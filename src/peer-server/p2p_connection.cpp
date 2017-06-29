#include "p2p_connection.h"

#include "logging.h"
#include "game.h"
#include "server.h"
#include "peer_db.h"
#include "random.h"

#include "humblenet_utils.h"

#include <sstream>

namespace humblenet {
	ha_bool P2PSignalConnection::processMsg(const HumblePeer::Message* msg)
	{
		auto msgType = msg->message_type();

		// we don't accept anything but HelloServer from a peer which hasn't sent one yet
		if (!this->peerId) {
			if (msgType != HumblePeer::MessageType::HelloServer) {
				LOG_WARNING("Got non-HelloServer message (%s) from non-authenticated peer \"%s\"\n", HumblePeer::EnumNameMessageType(msgType), this->url.c_str());
				// TODO: send error message
				return false;
			}
		}

		switch (msgType) {
			case HumblePeer::MessageType::P2POffer:
			{
				auto p2p = reinterpret_cast<const HumblePeer::P2POffer*>(msg->message());
				auto peer = p2p->peerId();

				// look up target peer, send message on
				// TODO: should ensure there's not already a negotiation going

				bool emulated = (p2p->flags() & 0x1);

				LOG_INFO("P2POffer from peer %u (%s) to peer %u", this->peerId, url.c_str(), peer);
				if (emulated) {
					LOG_INFO(", emulated connections not allowed\n");
					// TODO: better error message
					sendNoSuchPeer(this, peer);
					return true;
				}

				auto it = game->peers.find(peer);
				if (it == game->peers.end()) {
					LOG_WARNING(", no such peer\n");
					sendNoSuchPeer(this, peer);
				} else {
					P2PSignalConnection *otherPeer = it->second;
					assert(otherPeer != NULL);

					// if not emulated check if otherPeer supports WebRTC
					// to avoid unnecessary round trip
					if (!emulated && !otherPeer->webRTCsupport) {
						// webrtc connections not supported
						LOG_INFO("(%s), refusing because target doesn't support WebRTC\n", otherPeer->url.c_str());
						sendPeerRefused(this, peer);
						return true;
					}

					LOG_INFO("(%s)\n", otherPeer->url.c_str());

					// TODO: should check that it doesn't exist already
					this->connectedPeers.insert(otherPeer);

					// set peer id to originator so target knows who wants to connect
					sendP2PConnect(otherPeer, this->peerId, p2p->flags(), p2p->offer()->c_str());
				}

			}
				break;

			case HumblePeer::MessageType::P2PAnswer:
			{
				auto p2p = reinterpret_cast<const HumblePeer::P2PAnswer*>(msg->message());
				auto peer = p2p->peerId();

				// look up target peer, send message on
				// TODO: should only send if peers are currently negotiating connection

				auto it = game->peers.find(peer);

				if (it == game->peers.end()) {
					LOG_WARNING("P2PResponse from peer %u (%s) to nonexistent peer %u\n", this->peerId, this->url.c_str(), peer);
					sendNoSuchPeer(this, peer);
					return true;
				} else {
					P2PSignalConnection *otherPeer = it->second;
					assert(otherPeer != NULL);
					assert(otherPeer->peerId == peer);

					bool otherPeerConnected = (otherPeer->connectedPeers.find(this) != otherPeer->connectedPeers.end());
					if (!otherPeerConnected) {
						// we got P2PResponse but there's been no P2PConnect from
						// the peer we're supposed to respond to
						// client is either confused or malicious
						LOG_WARNING("P2PResponse from peer %u (%s) to peer %u (%s) who has not requested a P2P connection\n", this->peerId, this->url.c_str(), otherPeer->peerId, otherPeer->url.c_str());
						sendNoSuchPeer(this, peer);
						return true;
					}

					// TODO: should assert it's not there yet
					this->connectedPeers.insert(otherPeer);

					// set peer id to originator so target knows who wants to connect
					sendP2PResponse(otherPeer, this->peerId, p2p->offer()->c_str());
				}
			}
				break;

			case HumblePeer::MessageType::ICECandidate:
			{
				auto p2p = reinterpret_cast<const HumblePeer::ICECandidate*>(msg->message());
				auto peer = p2p->peerId();

				// look up target peer, send message on
				// TODO: should only send if peers are currently negotiating connection

				auto it = game->peers.find(peer);

				if (it == game->peers.end()) {
					LOG_WARNING(", no such peer\n");
					sendNoSuchPeer(this, peer);
				} else {
					P2PSignalConnection *otherPeer = it->second;
					assert(otherPeer != NULL);
					sendICECandidate(otherPeer, this->peerId, p2p->offer()->c_str());
				}

			}
				break;

			case HumblePeer::MessageType::P2PReject:
			{
				auto p2p = reinterpret_cast<const HumblePeer::P2PReject*>(msg->message());
				auto peer = p2p->peerId();

				auto it = game->peers.find(peer);
				// TODO: check there's such a connection attempt ongoing
				if (it == game->peers.end()) {
					switch (p2p->reason()) {
						case HumblePeer::P2PRejectReason::PeerRefused:
							LOG_WARNING("Peer %u (%s) tried to refuse connection from nonexistent peer %u\n", peerId, url.c_str(), peer);
							break;
						case HumblePeer::P2PRejectReason::NotFound:
							LOG_WARNING("Peer %u (%s) sent unexpected NotFound to non existent peer %u", peerId, url.c_str(), peer);
							break;
					}
					break;
				} else {
					P2PSignalConnection *otherPeer = it->second;
					assert(otherPeer != NULL);
					switch (p2p->reason()) {
						case HumblePeer::P2PRejectReason::PeerRefused:
							LOG_INFO("Peer %u (%s) refused connection from peer %u (%s)\n", this->peerId, this->url.c_str(), otherPeer->peerId, otherPeer->url.c_str());
							sendPeerRefused(otherPeer, this->peerId);
							break;
						case HumblePeer::P2PRejectReason::NotFound:
							LOG_WARNING("Peer %u (%s) setnt unexpected NotFound from peer %u (%s)\n", this->peerId, this->url.c_str(), otherPeer->peerId, otherPeer->url.c_str());
							sendPeerRefused(otherPeer, this->peerId);
							break;
					}
				}
			}
				break;

			case HumblePeer::MessageType::HelloServer:
			{
				auto hello = reinterpret_cast<const HumblePeer::HelloServer*>(msg->message());

				if (peerId != 0) {
					LOG_ERROR("Got hello HelloServer from client which already has a peer ID (%u)\n", peerId);
					return true;
				}

				if ((hello->flags() & 0x01) == 0) {
					LOG_ERROR("Client %s does not support WebRTC\n", url.c_str());
					return true;
				}

#pragma message ("TODO handle user authentication")

				this->game = peerServer->getVerifiedGame(hello);
				if (this->game == NULL) {
					// invalid game
					return false;
				}

				auto attributes = hello->attributes();
				const flatbuffers::String *platform = nullptr;
				if (attributes) {
					auto platform_lookup = attributes->LookupByKey("platform");
					if (platform_lookup) {
						platform = platform_lookup->value();
					}
				}

				auto reconnectToken = hello->reconnectToken();
				if( reconnectToken ) {
					PeerRecord record;

					if( ! peerServer->getPeerByToken(reconnectToken->c_str(), record ) ) {
						LOG_INFO("Got reconnect token from client that is no longer valid.\n");
						peerId = this->game->generateNewPeerId();
					} else if( record.game_id != this->game->gameId ) {
						LOG_INFO("Got reconnect token from client, but its not associated with the request game.\n");
						peerId = this->game->generateNewPeerId();
					} else {
						peerId = record.peer_id;

						LOG_INFO("Re-establishing state for peer: %u\n", peerId);

						for( auto it = record.aliases.begin(); it  != record.aliases.end(); ++it ) {
							this->game->aliases.insert( std::make_pair( *it, peerId ) );
						}

/*
						// transfer state from old instance to this one....
						auto sit = this->game->peers.find( peerId );
						if( sit != this->game->peers.end() ) {
							// copy connected peers
							this->connectedPeers.insert(sit->second->connectedPeers.begin(), sit->second->connectedPeers.end());

							// update all peers with new P2PSignalingConnection
							for( auto pit = this->game->peers.begin(); pit != this->game->peers.end(); ++pit ) {
								auto found = pit->second->connectedPeers.find( sit->second );
								if( found != pit->second->connectedPeers.end() ) {
									pit->second->connectedPeers.erase(found);
									pit->second->connectedPeers.insert(this);
								}
							}
						}
*/
					}

				} else {
					// generate peer id for this peer
					peerId = this->game->generateNewPeerId();
				}

				LOG_INFO("Got hello from \"%s\" (peer %u, game %u, platform: %s)\n", url.c_str(), peerId, this->game->gameId, platform ? platform->c_str(): "");

				game->peers.insert(std::make_pair(peerId, this));
				this->webRTCsupport = true;
				this->trickleICE = !(hello->flags() & 0x2);

				// send STUN/TURN server credential if client supports webrtc
				// ';' separator between server, username and password, like this:
				// "server;username;password"
				std::vector<ICEServer> iceServers;
				peerServer->populateStunServers(iceServers);

				// send hello to client
				this->reconnectToken = generate_random_hash(std::to_string(peerId));
				LOG_INFO("Reconnect token: %s\n", this->reconnectToken.c_str());

				sendHelloClient(this, peerId, this->reconnectToken, iceServers);
			}
				break;

			case HumblePeer::MessageType::HelloClient:
				// TODO: log address
				LOG_ERROR("Got hello HelloClient, not supposed to happen\n");
				break;

			case HumblePeer::MessageType::P2PConnected:
				// TODO: log address
				LOG_INFO("P2PConnect from peer %u\n", peerId);
				// TODO: clean up ongoing negotiations lit
				break;
			case HumblePeer::MessageType::P2PDisconnect:
				LOG_INFO("P2PDisconnect from peer %u\n", peerId);
				break;

			case HumblePeer::MessageType::P2PRelayData:
			{
				auto relay = reinterpret_cast<const HumblePeer::P2PRelayData*>(msg->message());
				auto peer = relay->peerId();
				auto data = relay->data();

				LOG_INFO("P2PRelayData relaying %d bytes from peer %u to %u\n", data->Length(), this->peerId, peer );

				auto it = game->peers.find(peer);

				if (it == game->peers.end()) {
					LOG_WARNING(", no such peer\n");
					sendNoSuchPeer(this, peer);
				} else {
					P2PSignalConnection *otherPeer = it->second;
					assert(otherPeer != NULL);
					sendP2PRelayData(otherPeer, this->peerId, data->Data(), data->Length() );
				}
			}
				break;

				// Alias processing
			case HumblePeer::MessageType::AliasRegister:
			{
				auto reg = reinterpret_cast<const HumblePeer::AliasRegister*>(msg->message());
				auto alias = reg->alias();

				auto existing = game->aliases.find( alias->c_str() );

				if( existing != game->aliases.end() && existing->second != peerId ) {
					LOG_INFO("Rejecting peer %u's request to register alias '%s' which is already registered to peer %u\n", peerId, alias->c_str(), existing->second );
	#pragma message ("TODO implement registration failure")
				} else {
					if( existing == game->aliases.end() ) {
						game->aliases.insert( std::make_pair( alias->c_str(), peerId ) );
					}
					LOG_INFO("Registering alias '%s' to peer %u\n", alias->c_str(), peerId );
	#pragma message ("TODO implement registration success")
				}
			}
				break;

			case HumblePeer::MessageType::AliasUnregister:
			{
				auto unreg = reinterpret_cast<const HumblePeer::AliasUnregister*>(msg->message());
				auto alias = unreg->alias();

				if (alias) {
					auto existing = game->aliases.find( alias->c_str() );

					if( existing != game->aliases.end() && existing->second == peerId ) {
						game->aliases.erase( existing );

						LOG_INFO("Unregistring alias '%s' for peer %u\n", alias->c_str(), peerId );
	#pragma message ("TODO implement unregister sucess")
					} else {
						LOG_INFO("Rejecting unregister of alias '%s' for peer %u\n", alias->c_str(), peerId );
	#pragma message ("TODO implement unregister failure")
					}
				} else {
					erase_value(game->aliases, peerId);

					LOG_INFO("Unregistring all aliases for peer for peer %u\n", peerId );
	#pragma message ("TODO implement unregister sucess")
				}
			}
				break;

			case HumblePeer::MessageType::AliasLookup:
			{
				auto lookup = reinterpret_cast<const HumblePeer::AliasLookup*>(msg->message());
				auto alias = lookup->alias();

				auto existing = game->aliases.find( alias->c_str() );

				if( existing != game->aliases.end() ) {
					LOG_INFO("Lookup of alias '%s' for peer %u resolved to peer %u\n", alias->c_str(), peerId, existing->second );
					sendAliasResolved(this, alias->c_str(), existing->second);
				} else {
					LOG_INFO("Lookup of alias '%s' for peer %u failed. No alias registered\n", alias->c_str(), peerId );
					sendAliasResolved(this, alias->c_str(), 0);
				}
			}
				break;

			default:
				LOG_WARNING("Unhandled P2P Message: %s\n", HumblePeer::EnumNameMessageType(msgType));
				break;
		}

		return true;
	}


	void P2PSignalConnection::sendMessage(const uint8_t *buff, size_t length) {
		bool wasEmpty = this->sendBuf.empty();
		this->sendBuf.insert(this->sendBuf.end(), buff, buff + length);
		if (wasEmpty) {
			peerServer->triggerWrite(this->wsi);
		}
	}

}
