#include "browser_terminal.h"

#include <Arduino.h>
#include <cerrno>
#include <cstring>
#include <esp_tls_crypto.h>
#include <sys/socket.h>

#include "network_transport.h"

namespace browser_terminal {
namespace {

constexpr size_t kMaxClients = 4;
// Terminal TX is admitted as one bounded frame so the UART path never needs
// an unbounded message buffer or a blocking WebSocket callback.
constexpr size_t kMaxTerminalPayload = 1024;
constexpr size_t kSerialChunkSize = 256;
constexpr size_t kReceiveBufferSize = 1056;
constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct Client {
    WiFiClient socket;
    uint8_t receiveBuffer[kReceiveBufferSize];
    size_t receiveLength;
    size_t fragmentedLength;
    bool binaryMessageInProgress;
    uint8_t outgoingFrame[kSerialChunkSize + 10];
    size_t outgoingLength;
    size_t outgoingOffset;
    bool canTransmit;
    bool active;
};

Client clients[kMaxClients];
uint8_t pendingSerialData[kSerialChunkSize];
size_t pendingSerialLength = 0;
portMUX_TYPE pendingLock = portMUX_INITIALIZER_UNLOCKED;

enum class SendResult : uint8_t {
    Sent = 0,
    Dropped,
    Broken,
};

enum class ParseResult : uint8_t {
    NeedMoreData = 0,
    Consumed,
    Close,
};

void closeClient(Client &client) {
    client.socket.stop();
    client.receiveLength = 0;
    client.fragmentedLength = 0;
    client.binaryMessageInProgress = false;
    client.outgoingLength = 0;
    client.outgoingOffset = 0;
    client.canTransmit = false;
    client.active = false;
}

SendResult sendFrame(
    Client &client,
    uint8_t opcode,
    const uint8_t *payload,
    size_t length) {
    if (payload == nullptr && length != 0) return SendResult::Broken;
    if (length > kSerialChunkSize) return SendResult::Broken;
    if (client.outgoingLength != 0) return SendResult::Dropped;

    size_t headerLength = 2;
    client.outgoingFrame[0] = static_cast<uint8_t>(0x80 | (opcode & 0x0F));
    if (length < 126) {
        client.outgoingFrame[1] = static_cast<uint8_t>(length);
    } else {
        client.outgoingFrame[1] = 126;
        client.outgoingFrame[2] = static_cast<uint8_t>(length >> 8);
        client.outgoingFrame[3] = static_cast<uint8_t>(length);
        headerLength = 4;
    }
    if (length != 0) memcpy(client.outgoingFrame + headerLength, payload, length);
    client.outgoingLength = headerLength + length;
    client.outgoingOffset = 0;

    const int descriptor = client.socket.fd();
    if (descriptor < 0) return SendResult::Broken;
    const ssize_t sent = send(
        descriptor,
        client.outgoingFrame,
        client.outgoingLength,
        MSG_DONTWAIT);
    if (sent > 0) client.outgoingOffset = static_cast<size_t>(sent);
    if (client.outgoingOffset == client.outgoingLength) {
        client.outgoingLength = 0;
        client.outgoingOffset = 0;
        return SendResult::Sent;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return SendResult::Dropped;
    }
    if (sent > 0) return SendResult::Dropped;
    return SendResult::Broken;
}

SendResult flushFrame(Client &client) {
    if (client.outgoingLength == 0) return SendResult::Sent;
    const int descriptor = client.socket.fd();
    if (descriptor < 0) return SendResult::Broken;
    const size_t remaining = client.outgoingLength - client.outgoingOffset;
    const ssize_t sent = send(
        descriptor,
        client.outgoingFrame + client.outgoingOffset,
        remaining,
        MSG_DONTWAIT);
    if (sent > 0) {
        client.outgoingOffset += static_cast<size_t>(sent);
        if (client.outgoingOffset == client.outgoingLength) {
            client.outgoingLength = 0;
            client.outgoingOffset = 0;
            return SendResult::Sent;
        }
        return SendResult::Dropped;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return SendResult::Dropped;
    }
    return SendResult::Broken;
}

bool readAvailable(Client &client) {
    const int available = client.socket.available();
    if (available <= 0) return true;
    if (client.receiveLength == sizeof(client.receiveBuffer)) return false;

    const size_t capacity = sizeof(client.receiveBuffer) - client.receiveLength;
    const size_t amount = min(capacity, static_cast<size_t>(available));
    const int read = client.socket.read(client.receiveBuffer + client.receiveLength, amount);
    if (read <= 0) return false;
    client.receiveLength += static_cast<size_t>(read);
    return true;
}

bool binaryFrameAccepted(const Client &client, const uint8_t *data, size_t length) {
    if (!client.canTransmit) return false;
    return network_transport::submitTerminalToSerial(data, length);
}

ParseResult parseOneFrame(Client &client) {
    const size_t messagePrefix = client.binaryMessageInProgress ?
        client.fragmentedLength : 0;
    if (client.receiveLength < messagePrefix + 2) return ParseResult::NeedMoreData;

    const uint8_t first = client.receiveBuffer[messagePrefix];
    const uint8_t second = client.receiveBuffer[messagePrefix + 1];
    if ((first & 0x70) != 0 || (second & 0x80) == 0) {
        return ParseResult::Close;
    }

    const uint8_t opcode = first & 0x0F;
    const bool finalFrame = (first & 0x80) != 0;
    const uint8_t lengthCode = second & 0x7F;
    size_t headerLength = 2;
    uint64_t payloadLength = lengthCode;
    if (lengthCode == 126) {
        headerLength += 2;
        if (client.receiveLength < messagePrefix + headerLength) {
            return ParseResult::NeedMoreData;
        }
        payloadLength = (static_cast<uint64_t>(client.receiveBuffer[messagePrefix + 2]) << 8) |
            client.receiveBuffer[messagePrefix + 3];
    } else if (lengthCode == 127) {
        headerLength += 8;
        if (client.receiveLength < messagePrefix + headerLength) {
            return ParseResult::NeedMoreData;
        }
        payloadLength = 0;
        for (size_t i = 0; i < 8; ++i) {
            payloadLength = (payloadLength << 8) |
                client.receiveBuffer[messagePrefix + 2 + i];
        }
    }
    if (payloadLength > kMaxTerminalPayload) {
        const uint8_t tooLarge[] = {0x03, 0xF1};
        sendFrame(client, 0x08, tooLarge, sizeof(tooLarge));
        closeClient(client);
        return ParseResult::Close;
    }

    headerLength += 4;
    const size_t frameLength = headerLength + static_cast<size_t>(payloadLength);
    if (client.receiveLength < messagePrefix + frameLength) {
        return ParseResult::NeedMoreData;
    }

    uint8_t *mask = client.receiveBuffer + messagePrefix + headerLength - 4;
    uint8_t *payload = client.receiveBuffer + messagePrefix + headerLength;
    for (size_t i = 0; i < payloadLength; ++i) payload[i] ^= mask[i % 4];

    if (opcode == 0x02) {
        if (client.binaryMessageInProgress) return ParseResult::Close;
        if (finalFrame) {
            binaryFrameAccepted(client, payload, static_cast<size_t>(payloadLength));
        } else {
            memmove(client.receiveBuffer, payload, static_cast<size_t>(payloadLength));
            client.fragmentedLength = static_cast<size_t>(payloadLength);
            client.binaryMessageInProgress = true;
        }
    } else if (opcode == 0x00) {
        if (!client.binaryMessageInProgress ||
                client.fragmentedLength + payloadLength > kMaxTerminalPayload) {
            return ParseResult::Close;
        }
        memmove(
            client.receiveBuffer + client.fragmentedLength,
            payload,
            static_cast<size_t>(payloadLength));
        client.fragmentedLength += static_cast<size_t>(payloadLength);
        if (finalFrame) {
            binaryFrameAccepted(client, client.receiveBuffer, client.fragmentedLength);
            client.fragmentedLength = 0;
            client.binaryMessageInProgress = false;
        }
    } else if (opcode == 0x08) {
        if (!finalFrame || payloadLength > 125) return ParseResult::Close;
        sendFrame(client, 0x08, nullptr, 0);
        closeClient(client);
        return ParseResult::Close;
    } else if (opcode == 0x09) {
        if (!finalFrame || payloadLength > 125) return ParseResult::Close;
        if (sendFrame(client, 0x0A, payload, static_cast<size_t>(payloadLength)) ==
                SendResult::Broken) {
            closeClient(client);
            return ParseResult::Close;
        }
    } else if (opcode != 0x0A) {
        return ParseResult::Close;
    } else if (!finalFrame || payloadLength > 125) {
        return ParseResult::Close;
    }

    const size_t frameEnd = messagePrefix + frameLength;
    const size_t remaining = client.receiveLength - frameEnd;
    const size_t retainedPrefix = client.binaryMessageInProgress ?
        client.fragmentedLength : 0;
    if (remaining != 0) {
        memmove(
            client.receiveBuffer + retainedPrefix,
            client.receiveBuffer + frameEnd,
            remaining);
    }
    client.receiveLength = retainedPrefix + remaining;
    return ParseResult::Consumed;
}

void serviceClient(Client &client) {
    if (!client.active) return;
    if (!client.socket.connected() || flushFrame(client) == SendResult::Broken ||
            !readAvailable(client)) {
        closeClient(client);
        return;
    }

    while (client.active && client.receiveLength >= 2) {
        switch (parseOneFrame(client)) {
            case ParseResult::NeedMoreData:
                return;
            case ParseResult::Close:
                if (client.active) closeClient(client);
                return;
            case ParseResult::Consumed:
                break;
        }
    }
}

void deliverPendingSerialData() {
    uint8_t data[kSerialChunkSize];
    size_t length = 0;
    portENTER_CRITICAL(&pendingLock);
    if (pendingSerialLength != 0) {
        length = pendingSerialLength;
        memcpy(data, pendingSerialData, length);
        pendingSerialLength = 0;
    }
    portEXIT_CRITICAL(&pendingLock);
    if (length == 0) return;

    for (Client &client : clients) {
        if (!client.active) continue;
        const SendResult result = sendFrame(client, 0x02, data, length);
        if (result == SendResult::Broken) closeClient(client);
    }
}

}  // namespace

void begin() {
    for (Client &client : clients) {
        client.receiveLength = 0;
        client.fragmentedLength = 0;
        client.binaryMessageInProgress = false;
        client.outgoingLength = 0;
        client.outgoingOffset = 0;
        client.canTransmit = false;
        client.active = false;
    }
    portENTER_CRITICAL(&pendingLock);
    pendingSerialLength = 0;
    portEXIT_CRITICAL(&pendingLock);
}

bool accept(WiFiClient client, const char *webSocketKey, bool allowTransmit) {
    if (!client || webSocketKey == nullptr || webSocketKey[0] == '\0') return false;

    Client *slot = nullptr;
    for (Client &candidate : clients) {
        if (!candidate.active) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) return false;

    char keyWithGuid[128];
    const int keyLength = snprintf(keyWithGuid, sizeof(keyWithGuid), "%s%s",
        webSocketKey, kWebSocketGuid);
    if (keyLength <= 0 || static_cast<size_t>(keyLength) >= sizeof(keyWithGuid)) return false;

    uint8_t digest[20];
    if (esp_crypto_sha1(reinterpret_cast<const unsigned char *>(keyWithGuid),
            static_cast<size_t>(keyLength), digest) != 0) return false;
    unsigned char encoded[29];
    size_t encodedLength = 0;
    if (esp_crypto_base64_encode(encoded, sizeof(encoded), &encodedLength, digest, sizeof(digest)) != 0 ||
            encodedLength >= sizeof(encoded)) return false;
    encoded[encodedLength] = '\0';

    const String response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + String(reinterpret_cast<char *>(encoded)) + "\r\n\r\n";
    if (client.write(reinterpret_cast<const uint8_t *>(response.c_str()), response.length()) !=
            response.length()) {
        client.stop();
        return false;
    }

    client.setNoDelay(true);
    slot->socket = client;
    slot->receiveLength = 0;
    slot->canTransmit = allowTransmit;
    slot->active = true;
    return true;
}

void service() {
    deliverPendingSerialData();
    for (Client &client : clients) serviceClient(client);
}

void onSerialData(const uint8_t *data, size_t length) {
    if (data == nullptr || length == 0 || length > kSerialChunkSize) return;

    // One bounded staging frame keeps WebSocket delivery out of the UART callback.
    // A busy browser loses only its observation copy; TCP and UART capture continue.
    portENTER_CRITICAL(&pendingLock);
    if (pendingSerialLength == 0) {
        memcpy(pendingSerialData, data, length);
        pendingSerialLength = length;
    }
    portEXIT_CRITICAL(&pendingLock);
}

}  // namespace browser_terminal
