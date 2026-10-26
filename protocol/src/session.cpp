#include "video_datagram.h"
#include "video_control.h"

#include "session.h"
#include "serializer.h"
#include "protocol.h"
#include "packet_builder.h"
#include "version_whitelist.h"

#include "video_payload.h"
#include "utils.h"

#include <stdexcept>
#include <utility>
#include <array>
#include <algorithm>

// TODO: Negotiate features. This should be done at first and ignore subsequently

namespace camdroid::protocol {

    namespace utils {

        constexpr std::array<std::uint8_t, 7> terminal_fin_payload{
            static_cast<std::uint8_t>(CommunicationOpcode::Handshake),
            0x00,
            0xAE,
            0xEF,

            0x00,
            0x11,
            0xFF
        };

    }

    session::session() {
        return;
    }

    void session::start() {

        if (state != SessionState::Disconnected)
            throw std::runtime_error("Protocol session failure: Session already started.");

        state = SessionState::Handshaking;

        return;

    }

    void session::feed(const void* data, std::size_t size) {

        if (state == SessionState::Disconnected || state == SessionState::Closed)
            throw std::runtime_error("Protocol session failure: Session is not active.");

        receiver.feed(data, size);

        return;

    }

    std::optional<packet> session::next_packet() {

        if (state == SessionState::Disconnected || state == SessionState::Closed)
            throw std::runtime_error("Protocol session failure: Session is not active.");

        // Keep early peer data buffered until the terminal FIN write completes.
        // It may already use the negotiated framing.
        if (terminal_fin_pending)
            return std::nullopt;

        auto pkt = receiver.next_packet();

        if (!pkt.has_value())
            return std::nullopt;

        validate_packet(*pkt);

        if (pkt->type == PacketType::Communication)
            handle_communication(*pkt);
        else if (pkt->type == PacketType::Video)
            handle_video(*pkt);

        return pkt;

    }

    std::vector<std::uint8_t> session::send(const packet& pkt) {

        if (state == SessionState::Disconnected || state == SessionState::Closed)
            throw std::runtime_error("Protocol session failure: Session is not active.");

        validate_packet(pkt);

        if (pkt.type == PacketType::Communication && pkt.has_flag(ProtocolFlag::NACK))
            throw std::runtime_error("Protocol session failure: Outgoing Communication packets cannot use NACK.");

        if (pkt.has_flag(ProtocolFlag::OPTS) != outgoing_optimized)
            throw std::runtime_error("Protocol session failure: Invalid outgoing framing mode.");

        auto bytes = serializer::serialize(pkt);

        // Remember our initial offer just as the receiver does. Acceptance may
        // contain only the handshake opcode, so it cannot repeat this setting.
        // Keep framing unchanged until the terminal FIN write completes.
        if (state == SessionState::Handshaking && pending_negotiations == 0 && pkt.type == PacketType::Communication &&
            pkt.flags == static_cast<std::uint8_t>(ProtocolFlag::QRY) && pkt.payload.size() >= 4 &&
            pkt.payload[0] == static_cast<std::uint8_t>(CommunicationOpcode::Handshake) && pkt.payload[1] <= pkt.payload[2]) {

            optimization_pending = (pkt.payload[3] & (1U << 7)) != 0;

        }

        return bytes;

    }

    SessionState session::get_state() const noexcept {
        return state;
    }

    bool session::is_optimized() const noexcept {
        return incoming_optimized && outgoing_optimized;
    }

    void session::close() {

        receiver.reset();

        incoming_optimized = false;
        outgoing_optimized = false;

        peer_supports_optimization = false;
        optimization_pending = false;

        negotiated_version = version;
        terminal_fin_pending = false;

        pending_version = version;
        pending_negotiations = 0;
        pending_counteroffers = 0;

        state = SessionState::Closed;

        return;

    }

    void session::validate_packet(const packet& pkt) {

        bool ack = pkt.has_flag(ProtocolFlag::ACK);
        bool err = pkt.has_flag(ProtocolFlag::ERR);
        bool qry = pkt.has_flag(ProtocolFlag::QRY);
        bool nack = pkt.has_flag(ProtocolFlag::NACK);
        bool fin = pkt.has_flag(ProtocolFlag::FIN);

        // Reserved protocol flag bits
        if ((pkt.flags & 0xC0) != 0)
            throw std::runtime_error("Protocol validation failure: Reserved protocol flags are set.");

        // NACK means no acknowledgement is requested.
        if (qry && nack && pkt.type != PacketType::Communication)
            throw std::runtime_error("Protocol validation failure: QRY and NACK cannot coexist.");

        // FIN can only coexist with ACK, OPTS and NACK.
        if (fin && (err || qry))
            throw std::runtime_error("Protocol validation failure: Invalid FIN flag combination.");

        // Handshake only permits communication packets.
        if ((state == SessionState::Handshaking || state == SessionState::Finalizing) &&
            pkt.type != PacketType::Communication)
            throw std::runtime_error("Protocol validation failure: Non-communication packet during handshake.");

        // Validate version on normal packets.
        if (state == SessionState::Established && !pkt.has_flag(ProtocolFlag::OPTS) && pkt.version != negotiated_version)
            throw std::runtime_error("Protocol validation failure: Version mismatch.");

        return;

    }

    void session::handle_video(const packet& pkt) {

        // All video packets are NACK by default.

        try {

            video_payload::validate(pkt);

            switch (pkt.payload_flags) {

                case static_cast<std::uint8_t>(VideoFlag::CONFIG): {

                    /* Configuration of decoder */
                    video_state = VideoState::AwaitingKeyframe;
                    break;
                }

                case static_cast<std::uint8_t>(VideoFlag::KEYFRAME): {

                    /* Video footage */
                    if (this->video_state != VideoState::AwaitingKeyframe && this->video_state != VideoState::Streaming)
                        return;

                    this->video_state = VideoState::Streaming;

                    break;
                }

                case 0: {
                    if (this->video_state != VideoState::Streaming)
                        throw std::invalid_argument("Frame received before keyframe or after EOS.");

                    break;
                }

                case static_cast<std::uint8_t>(VideoFlag::EOS): {

                    /* End of stream */
                    this->video_state = VideoState::Ended;
                    break;
                }

            }

        } catch (const std::invalid_argument& error) {

            this->last_error = std::string("Protocol video failure: ") + error.what();
            // Propagate the failure so next_packet() does not return invalid video
            // to the decoder. This is not a handshake rejection.
            throw std::runtime_error(this->last_error);

        }

    }

    void session::handle_communication(const packet& pkt) {

        if (pkt.payload.size() < 1) {
            reject_handshake("Invalid payload for packet.");
            return;
        }

        if (state == SessionState::Established && pkt.payload[0] >= video_datagram::setup &&
            pkt.payload[0] <= video_datagram::recover) {

            video_datagram::validate(pkt);
            return;

        }

        if (pkt.payload[0] == 0x15 && state == SessionState::Established) {

            (void)parse_video_orientation(pkt);
            return;

        }

        if (pkt.payload[0] == 0x13 && state == SessionState::Established) {

            (void)parse_camera_capabilities(pkt);
            return;

        }

        if (pkt.payload[0] == 0x14 && state == SessionState::Established) {

            (void)parse_capture_selection(pkt);
            return;

        }

        if (pkt.payload[0] == 0x12 && state == SessionState::Established) {

            (void)parse_video_feedback(pkt);
            return;

        }

        if (pkt.payload[0] == 0x11 && state == SessionState::Established) {

            (void)parse_camera_flip(pkt);
            return;

        }

        if (pkt.payload[0] == 0x10 && state == SessionState::Established) {

            (void)parse_video_control(pkt);

            return;

        }

        if ((pkt.payload.at(0) == static_cast<std::uint8_t>(CommunicationOpcode::Handshake)) &&
            (this->state == SessionState::Handshaking || this->state == SessionState::Finalizing)) {

            handle_handshake(pkt);

            return;

        } else {

            reject_handshake("Unknown OpCode.");
            return;

        }

        return;

    }

    void session::handle_handshake(const packet& pkt) {

        switch (pkt.flags) {

            // Only QRY is set
            case (1U << 2): {
                // Initializer of the handshake

                if (this->state != SessionState::Handshaking || pending_negotiations) {
                    reject_handshake("A handshake is in progress.");
                    return;
                }

                if (pkt.payload.size() < 4) {
                    reject_handshake("Protocol handshake failure: Initial handshake requires four bytes.");
                    return;
                }

                if (pkt.payload.at(1) > pkt.payload.at(2)) {
                    reject_handshake("Protocol handshake failure: Invalid version range.");
                    return;
                }

                packet_builder response_request_negotiation(camdroid::protocol::version);
                std::vector<std::uint8_t> response_request_negotiation_payload;
                response_request_negotiation_payload.push_back(static_cast<uint8_t>(CommunicationOpcode::Handshake));
                response_request_negotiation.type(PacketType::Communication);

                bool is_negotiation_ready = false;

                if (pkt.version != camdroid::protocol::version) {

                    // Negotiate versions.

                    std::uint8_t min_version = pkt.payload.at(1);
                    std::uint8_t max_version = pkt.payload.at(2);

                    if ((camdroid::protocol::version <= max_version) && (camdroid::protocol::version >= min_version)) {

                        // request a negotiation

                        response_request_negotiation.flag(ProtocolFlag::ACK).flag(ProtocolFlag::QRY);

                        // Version negotiations request
                        response_request_negotiation_payload.push_back(0x1);
                        response_request_negotiation_payload.push_back(camdroid::protocol::min_supported_version);
                        response_request_negotiation_payload.push_back(camdroid::protocol::max_supported_version);

                        is_negotiation_ready = true;
                        pending_negotiations |= 0x1;

                    } else {

                        // Cannot negotiate versions.
                        reject_handshake(
                            "Cannot negotiate versions. My offer: " + std::to_string(camdroid::protocol::version)
                        );

                        return;

                    }

                } else {

                    this->pending_version = camdroid::protocol::version;

                }

                // Do we need to switch to optimization mode?
                if (pkt.payload.at(3) & (1U << 7)) {

                    // Negotiate if we do not want optimization here.
                    // Right now, it doesn't matter.
                    this->optimization_pending = true;

                }

                if (is_negotiation_ready) {

                    // Someone already set flags and prepared payload. Just send it.
                    outgoing_responses.push_back(
                        response_request_negotiation.payload(response_request_negotiation_payload).build()
                    );

                } else {

                    // Accept offers
                    outgoing_responses.push_back(response_request_negotiation.flag(ProtocolFlag::ACK)
                                                     .flag(ProtocolFlag::FIN)
                                                     .payload(response_request_negotiation_payload)
                                                     .build());
                    this->state = SessionState::Finalizing;

                }

                break;
            }

            // ACK + QRY is set
            case ((1U << 2) | (0x1)): {
                // Respond to a negotiation

                if (pkt.payload.size() < 2) {
                    reject_handshake("Invalid handshake packet.");
                    return;
                }

                std::uint8_t reason = pkt.payload.at(1);
                std::vector<std::uint8_t> response_payload;

                response_payload.push_back(static_cast<uint8_t>(CommunicationOpcode::Handshake));
                bool is_response = (reason & (1U << 7)) != 0;

                const std::uint8_t requested = reason & 0x7FU;

                if (requested & ~0x03U) {
                    reject_handshake("Protocol handshake failure: Unknown negotiation fields.");
                    return;
                }

                const std::size_t expected_size =
                    2 + ((requested & 0x01U) ? (is_response ? 1 : 2) : 0) + ((requested & 0x02U) ? 1 : 0);

                if (pkt.payload.size() != expected_size) {
                    reject_handshake("Protocol handshake failure: Invalid negotiation payload size.");
                    return;
                }

                if ((requested & 0x02U) && pkt.payload.back() > 1) {
                    reject_handshake("Protocol handshake failure: Invalid optimization state.");
                    return;
                }

                std::uint8_t proposed_version = this->pending_version;
                bool proposed_optimization = this->optimization_pending;
                std::size_t offset = 2;

                if (is_response) {

                    std::uint8_t requested = reason & 0x7FU;

                    if (requested != this->pending_negotiations) {
                        reject_handshake("Protocol handshake failure: Unexpected negotiation fields.");
                        return;
                    }

                    try {

                        if (reason & (0x1U << 0)) {

                            // Version negotiation accepted
                            std::uint8_t accepted_negotiation_version = pkt.payload.at(offset++);

                            // All good.
                            if ((this->pending_version == accepted_negotiation_version) ||
                                (supported_versions.contains(accepted_negotiation_version) &&
                                 accepted_negotiation_version <= max_supported_version &&
                                 accepted_negotiation_version >= min_supported_version))
                                proposed_version = accepted_negotiation_version;

                            else {
                                reject_handshake("Cannot negotiate versions.");
                                return;
                            }

                        }

                        if (reason & (0x1U << 1)) {

                            // Optimization negotiation accepted
                            std::uint8_t accepted_negotiation_optimized = pkt.payload.at(offset++);
                            proposed_optimization = accepted_negotiation_optimized != 0;

                        }

                    } catch (const std::out_of_range& e) {
                        reject_handshake(std::string("Invalid handshake packet: Missing some required fields.") + e.what());
                        return;
                    }

                    this->pending_version = proposed_version;
                    this->optimization_pending = proposed_optimization;
                    this->pending_negotiations = 0;

                    // Send a successful response
                    packet_builder response_packet_builder(negotiated_version);
                    response_packet_builder.type(PacketType::Communication).flag(ProtocolFlag::ACK).flag(ProtocolFlag::FIN);

                    response_packet_builder.payload(response_payload);
                    packet response_packet = response_packet_builder.build();
                    // Send that packet
                    outgoing_responses.push_back(std::move(response_packet));
                    this->state = SessionState::Finalizing;

                } else {

                    bool negotiation_failed = false;
                    bool offer_negotiation = false;
                    std::uint8_t negotiation_failure_cause = 0x00U;

                    if (reason & (0x1U << 0)) {

                        // Version negotiation requested

                        if (pkt.payload.size() < offset + 2) {
                            reject_handshake("Missing some fields.");
                            return;
                        }

                        std::uint8_t min_negotiator_version = pkt.payload.at(offset++);
                        std::uint8_t max_negotiator_version = pkt.payload.at(offset++);

                        if (max_negotiator_version < min_negotiator_version) {
                            reject_handshake("Invalid handshake request.");
                            return;
                        }

                        auto best_match =
                            supported_versions.find(pkt.version, min_negotiator_version, max_negotiator_version);

                        if (best_match.has_value()) {

                            proposed_version = *best_match;

                        } else {

                            negotiation_failed = true;
                            // Version failure. This always results in a termination.
                            negotiation_failure_cause |= 0x1;

                        }

                    }

                    if (!negotiation_failed && reason & (0x1U << 1)) {

                        // Optimization negotiation

                        if (pkt.payload.size() < offset + 1) {
                            reject_handshake("Missing some fields.");
                            return;
                        }

                        std::uint8_t optimization_status_requested = pkt.payload.at(offset++);

                        proposed_optimization = optimization_status_requested != 0;
                        // Currently always permitted but if user config controls it later on
                        // then ig offer_negotiation can append a flag onto the bit and a different payload option

                    }

                    packet_builder response_packet_builder(camdroid::protocol::version);
                    response_packet_builder.type(PacketType::Communication);

                    if (negotiation_failed) {
                        reject_handshake("Failed to negotiate.");
                        return;
                    } else {

                        this->pending_version = proposed_version;
                        this->optimization_pending = proposed_optimization;
                        response_payload.push_back(reason | (1U << 7));

                        if (offer_negotiation) {

                            // Currently un-reachable code.
                            response_packet_builder.flag(ProtocolFlag::ERR);
                            response_payload.push_back(negotiation_failure_cause);

                        }

                        if (reason & (1U << 0))
                            response_payload.push_back(this->pending_version);

                        if (reason & (1U << 1))
                            response_payload.push_back(optimization_pending);

                        response_packet_builder.flag(ProtocolFlag::ACK).flag(ProtocolFlag::QRY);

                    }

                    response_packet_builder.payload(response_payload);
                    packet response_packet = response_packet_builder.build();

                    // Send that packet
                    outgoing_responses.push_back(std::move(response_packet));

                }

                break;
            }

            // ACK + FIN is set
            case ((1U << 5) | (0x1)): {
                // Respond to an acceptance

                if (pending_negotiations) {

                    reject_handshake("Unexpected FIN.");
                    return;

                }

                switch (this->state) {

                    case SessionState::Handshaking: {

                        packet_builder response_packet_builder(negotiated_version);
                        response_packet_builder.type(PacketType::Communication)
                            .flag(ProtocolFlag::ACK)
                            .flag(ProtocolFlag::FIN);

                        std::vector<std::uint8_t> response_payload;
                        response_payload.push_back(static_cast<uint8_t>(CommunicationOpcode::Handshake));

                        // Mark this FIN as terminal
                        response_payload.push_back(0x00);
                        response_payload.push_back(0xAE);
                        response_payload.push_back(0xEF);
                        response_payload.push_back(0x00);

                        response_payload.push_back(0x11);
                        response_payload.push_back(0xFF);

                        response_packet_builder.payload(response_payload);

                        packet response_packet = response_packet_builder.build();
                        // Send that packet
                        outgoing_responses.push_back(std::move(response_packet));

                        this->state = SessionState::Finalizing;
                        this->terminal_fin_pending = true;

                        break;
                    }

                    case SessionState::Finalizing: {

                        if (this->terminal_fin_pending) {
                            reject_handshake("Unexpected FIN while sending final confirmation.");
                            return;
                        }

                        if (pkt.payload.size() != utils::terminal_fin_payload.size() ||
                            !std::equal(pkt.payload.begin(), pkt.payload.end(), utils::terminal_fin_payload.begin())) {
                            reject_handshake("Protocol handshake failure: Invalid terminal FIN payload.");
                            return;
                        }

                        this->negotiated_version = this->pending_version;
                        this->incoming_optimized = this->optimization_pending;
                        this->outgoing_optimized = this->optimization_pending;
                        this->receiver.set_optimized(this->incoming_optimized);
                        this->state = SessionState::Established;

                        break;
                    }

                    case SessionState::Established:
                    // Final handshake
                    case SessionState::Closed:
                    default:
                        break;

                }

                break;
            }

            // ACK + ERR is set
            case ((1U << 1) | (0x1)): {
                // Negotiations failed
                // Peer has rejected the handshake.
                // Payload: [Handshake opcode][UTF-8 error message...]

                std::string error_message;

                if (pkt.payload.size() > 1) {
                    error_message.assign(pkt.payload.begin() + 1, pkt.payload.end());
                } else {
                    error_message = "Peer rejected the handshake without providing a reason.";
                }

                this->last_error = "Protocol handshake failure: " + error_message;

                this->state = SessionState::Closing;

                this->pending_negotiations = 0;
                this->pending_counteroffers = 0;
                this->terminal_fin_pending = false;
                this->optimization_pending = false;

                // Do not send another ACK | ERR.
                // The peer has already indicated a terminal failure.
                break;
            }

            // ACK + ERR + QRY is set
            case ((1U << 2) | (1U << 1) | 0x1U): {
                // Peer has returned a counterproposal for one or more negotiation fields.

                if (this->state != SessionState::Handshaking) {
                    reject_handshake("Protocol handshake failure: Unexpected counterproposal.");
                    return;
                }

                if (pkt.payload.size() < 3) {
                    reject_handshake("Protocol handshake failure: Invalid counterproposal payload.");
                    return;
                }

                std::uint8_t reason = pkt.payload.at(1);
                std::uint8_t failure_mask = pkt.payload.at(2);

                if (!(reason & (1U << 7))) {
                    reject_handshake("Protocol handshake failure: Expected negotiation response.");
                    return;
                }

                std::uint8_t requested = reason & 0x7FU;

                if (requested & ~0x03U) {
                    reject_handshake("Protocol handshake failure: Unknown negotiation fields.");
                    return;
                }

                if (!failure_mask || (failure_mask & ~requested)) {
                    reject_handshake("Protocol handshake failure: Invalid counterproposal mask.");
                    return;
                }

                if (requested != this->pending_negotiations) {
                    reject_handshake("Protocol handshake failure: Unexpected negotiation fields.");
                    return;
                }

                std::size_t expected_size = 3;

                if (requested & (1U << 0))
                    expected_size++;

                if (requested & (1U << 1))
                    expected_size++;

                if (pkt.payload.size() != expected_size) {
                    reject_handshake("Protocol handshake failure: Invalid counterproposal payload size.");
                    return;
                }

                std::size_t offset = 3;

                std::uint8_t proposed_version = this->pending_version;
                bool proposed_optimization = this->optimization_pending;

                if (requested & (1U << 0)) {

                    // Selected or counterproposed protocol version.
                    proposed_version = pkt.payload.at(offset++);

                    if (!supported_versions.contains(proposed_version)) {
                        reject_handshake("Protocol handshake failure: Unsupported counterproposed version.");
                        return;
                    }

                }

                if (requested & (1U << 1)) {

                    // Selected or counterproposed optimization state.
                    std::uint8_t optimization_value = pkt.payload.at(offset++);

                    if (optimization_value > 1) {
                        reject_handshake("Protocol handshake failure: Invalid optimization state.");
                        return;
                    }

                    proposed_optimization = optimization_value != 0;

                }

                // All fields validated. Apply the proposed configuration as pending.
                this->pending_version = proposed_version;
                this->optimization_pending = proposed_optimization;
                this->pending_negotiations = 0;
                this->pending_counteroffers = failure_mask;

                // Accept the counterproposal by confirming the complete selected configuration.
                std::vector<std::uint8_t> response_payload;
                response_payload.reserve(2 + ((requested & 0x01U) != 0) + ((requested & 0x02U) != 0));

                response_payload.push_back(static_cast<std::uint8_t>(CommunicationOpcode::Handshake));

                response_payload.push_back(requested);

                if (requested & (1U << 0))
                    response_payload.push_back(this->pending_version);

                if (requested & (1U << 1))
                    response_payload.push_back(static_cast<std::uint8_t>(this->optimization_pending));

                packet_builder response_packet_builder(camdroid::protocol::version);

                response_packet_builder.type(PacketType::Communication)
                    .flag(ProtocolFlag::ACK)
                    .flag(ProtocolFlag::FIN)
                    .payload(std::move(response_payload));

                this->outgoing_responses.push_back(response_packet_builder.build());
                this->state = SessionState::Finalizing;

                break;
            }

            default:
                reject_handshake("Invalid handshake packet");
                return;

        }

        return;

    }

    void session::reject_handshake(const std::string& message) {

        std::vector<std::uint8_t> payload;
        payload.reserve(1 + message.size());

        payload.push_back(static_cast<std::uint8_t>(CommunicationOpcode::Handshake));
        payload.insert(payload.end(), message.begin(), message.end());

        packet_builder builder(this->negotiated_version);

        builder.type(PacketType::Communication)
            .flag(ProtocolFlag::ACK)
            .flag(ProtocolFlag::ERR)
            .flag(ProtocolFlag::OPTS, this->outgoing_optimized)
            .payload(std::move(payload));

        this->outgoing_responses.push_back(builder.build());

        this->state = SessionState::Closing;
        this->last_error = message;

        return;

    }

    void session::on_terminal_fin_sent() {

        if (this->state != SessionState::Finalizing || !this->terminal_fin_pending)
            throw std::runtime_error("Protocol session failure: Unexpected terminal FIN completion.");

        this->terminal_fin_pending = false;
        this->negotiated_version = this->pending_version;
        this->incoming_optimized = this->optimization_pending;
        this->outgoing_optimized = this->optimization_pending;
        this->receiver.set_optimized(this->incoming_optimized);
        this->state = SessionState::Established;

        return;

    }

    std::optional<packet> session::next_response() {

        if (outgoing_responses.size() == 0)
            return std::nullopt;

        packet response = std::move(outgoing_responses.front());
        outgoing_responses.pop_front();

        return response;

    }

    bool session::is_terminal_fin_pending() const noexcept {
        return terminal_fin_pending;
    }

}