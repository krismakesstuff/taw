// The phone link's HTTPS: a key and a self-signed certificate, and TLS on a connection.

#include "PhoneTls.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <mutex>

namespace taw
{
struct PhoneTls::State
{
    State()
    {
        mbedtls_entropy_init (&entropy);
        mbedtls_ctr_drbg_init (&random);
        mbedtls_pk_init (&key);
        mbedtls_x509_crt_init (&certificate);
        mbedtls_ssl_config_init (&config);
    }

    ~State()
    {
        mbedtls_ssl_config_free (&config);
        mbedtls_x509_crt_free (&certificate);
        mbedtls_pk_free (&key);
        mbedtls_ctr_drbg_free (&random);
        mbedtls_entropy_free (&entropy);
    }

    // The generator is one for every connection's thread.
    static int randomBytes (void* self, unsigned char* to, size_t size)
    {
        auto& s = *static_cast<State*> (self);
        const std::lock_guard<std::mutex> lock (s.randomLock);
        return mbedtls_ctr_drbg_random (&s.random, to, size);
    }

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    std::mutex randomLock;
    mbedtls_pk_context key;
    mbedtls_x509_crt certificate;
    mbedtls_ssl_config config;
    bool ready = false;
};

struct PhoneTls::Line::State
{
    State() { mbedtls_ssl_init (&ssl); }
    ~State() { mbedtls_ssl_free (&ssl); }

    static int send (void* self, const unsigned char* from, size_t size)
    {
        auto& s = *static_cast<State*> (self);
        const int sent = s.socket->write (from, (int) std::min<size_t> (size, 1 << 20));
        return sent > 0 ? sent : MBEDTLS_ERR_SSL_CONN_EOF;
    }

    static int receive (void* self, unsigned char* to, size_t size)
    {
        auto& s = *static_cast<State*> (self);

        // What was read before anyone knew this was TLS.
        if (s.firstAt < s.first.getSize())
        {
            const auto n = std::min (size, s.first.getSize() - s.firstAt);
            std::memcpy (to, static_cast<const char*> (s.first.getData()) + s.firstAt, n);
            s.firstAt += n;
            return (int) n;
        }

        if (s.socket->waitUntilReady (true, s.timeoutMs) != 1)
            return MBEDTLS_ERR_SSL_WANT_READ; // nothing yet: asked again later

        const int got = s.socket->read (to, (int) std::min<size_t> (size, 1 << 20), false);
        return got > 0 ? got : MBEDTLS_ERR_SSL_CONN_EOF;
    }

    mbedtls_ssl_context ssl;
    juce::StreamingSocket* socket = nullptr;
    juce::MemoryBlock first;
    size_t firstAt = 0;
    int timeoutMs = 3000;
};

PhoneTls::PhoneTls() : state (std::make_unique<State>()) {}
PhoneTls::~PhoneTls() = default;
PhoneTls::Line::~Line()
{
    if (state != nullptr)
        mbedtls_ssl_close_notify (&state->ssl);
}

bool PhoneTls::prepare (const juce::File& folder, const juce::String& address, juce::String& error)
{
    auto& s = *state;
    const auto failed = [&error] (const char* what, int code)
    {
        error = juce::String ("no HTTPS (") + what + " " + juce::String (code) + ")";
        return false;
    };

    psa_crypto_init();
    const char* const who = "taw phone link";

    if (const int r = mbedtls_ctr_drbg_seed (&s.random, mbedtls_entropy_func, &s.entropy, reinterpret_cast<const unsigned char*> (who), std::strlen (who)); r != 0)
        return failed ("random", r);

    // Kept: the key, the certificate, and the address the certificate is for. A new address
    // (another network), or a certificate getting old, and they're made again.
    const auto keyFile = folder.getChildFile ("phone-key.pem"), certFile = folder.getChildFile ("phone-cert.pem"),
               forFile = folder.getChildFile ("phone-cert.txt");
    const bool kept = keyFile.existsAsFile() && certFile.existsAsFile() && forFile.loadFileAsString().trim() == address
                      && juce::Time::getCurrentTime() - certFile.getCreationTime() < juce::RelativeTime::days (600);

    const auto parse = [&]
    {
        const auto keyPem = keyFile.loadFileAsString(), certPem = certFile.loadFileAsString();
        return mbedtls_pk_parse_key (&s.key, reinterpret_cast<const unsigned char*> (keyPem.toRawUTF8()), keyPem.getNumBytesAsUTF8() + 1, nullptr, 0,
                                     State::randomBytes, &s) == 0
               && mbedtls_x509_crt_parse (&s.certificate, reinterpret_cast<const unsigned char*> (certPem.toRawUTF8()), certPem.getNumBytesAsUTF8() + 1) == 0;
    };

    if (! kept || ! parse())
    {
        mbedtls_pk_free (&s.key);
        mbedtls_pk_init (&s.key);
        mbedtls_x509_crt_free (&s.certificate);
        mbedtls_x509_crt_init (&s.certificate);

        if (const int r = mbedtls_pk_setup (&s.key, mbedtls_pk_info_from_type (MBEDTLS_PK_ECKEY)); r != 0)
            return failed ("key", r);

        if (const int r = mbedtls_ecp_gen_key (MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec (s.key), State::randomBytes, &s); r != 0)
            return failed ("key", r);

        mbedtls_x509write_cert writing;
        mbedtls_x509write_crt_init (&writing);
        mbedtls_x509write_crt_set_version (&writing, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg (&writing, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key (&writing, &s.key);
        mbedtls_x509write_crt_set_issuer_key (&writing, &s.key);

        unsigned char serial[16];
        State::randomBytes (&s, serial, sizeof (serial));
        serial[0] = (unsigned char) ((serial[0] & 0x7f) | 0x01); // positive, not zero

        // For this address (browsers go by the alternative name), for two years.
        unsigned char ip[4] = {};
        const auto parts = juce::StringArray::fromTokens (address, ".", "");

        for (int i = 0; i < 4; ++i)
            ip[i] = (unsigned char) parts[i].getIntValue();

        mbedtls_x509_san_list name {};
        name.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        name.node.san.unstructured_name.p = ip;
        name.node.san.unstructured_name.len = sizeof (ip);

        const auto now = juce::Time::getCurrentTime();
        const auto from = (now - juce::RelativeTime::days (1)).formatted ("%Y%m%d%H%M%S"), until = (now + juce::RelativeTime::days (730)).formatted ("%Y%m%d%H%M%S");
        const auto called = "CN=taw phone link " + address;
        unsigned char pem[4096];
        int r = mbedtls_x509write_crt_set_subject_name (&writing, called.toRawUTF8());
        r = r != 0 ? r : mbedtls_x509write_crt_set_issuer_name (&writing, called.toRawUTF8());
        r = r != 0 ? r : mbedtls_x509write_crt_set_serial_raw (&writing, serial, sizeof (serial));
        r = r != 0 ? r : mbedtls_x509write_crt_set_validity (&writing, from.toRawUTF8(), until.toRawUTF8());
        r = r != 0 ? r : mbedtls_x509write_crt_set_basic_constraints (&writing, 0, -1);
        r = r != 0 ? r : mbedtls_x509write_crt_set_subject_alternative_name (&writing, &name);
        r = r != 0 ? r : mbedtls_x509write_crt_pem (&writing, pem, sizeof (pem), State::randomBytes, &s);
        mbedtls_x509write_crt_free (&writing);

        if (r != 0)
            return failed ("certificate", r);

        const juce::String certPem (reinterpret_cast<const char*> (pem));

        if (r = mbedtls_pk_write_key_pem (&s.key, pem, sizeof (pem)); r != 0)
            return failed ("key", r);

        folder.createDirectory();
        certFile.deleteFile(); // made now: its age is counted from here
        certFile.replaceWithText (certPem);
        keyFile.replaceWithText (juce::String (reinterpret_cast<const char*> (pem)));
        forFile.replaceWithText (address);

        if (r = mbedtls_x509_crt_parse (&s.certificate, reinterpret_cast<const unsigned char*> (certPem.toRawUTF8()), certPem.getNumBytesAsUTF8() + 1); r != 0)
            return failed ("certificate", r);
    }

    if (const int r = mbedtls_ssl_config_defaults (&s.config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT); r != 0)
        return failed ("setup", r);

    mbedtls_ssl_conf_rng (&s.config, State::randomBytes, &s);
    mbedtls_ssl_conf_max_tls_version (&s.config, MBEDTLS_SSL_VERSION_TLS1_2); // every browser has it; 1.3 here wants more set up

    if (const int r = mbedtls_ssl_conf_own_cert (&s.config, &s.certificate, &s.key); r != 0)
        return failed ("setup", r);

    s.ready = true;
    return true;
}

std::unique_ptr<PhoneTls::Line> PhoneTls::accept (juce::StreamingSocket& socket, const void* first, int firstSize, const std::atomic<bool>& stopping)
{
    if (! state->ready)
        return nullptr;

    auto line = std::unique_ptr<Line> (new Line());
    line->state = std::make_unique<Line::State>();
    auto& l = *line->state;
    l.socket = &socket;
    l.first.append (first, (size_t) firstSize);

    if (mbedtls_ssl_setup (&l.ssl, &state->config) != 0)
        return nullptr;

    mbedtls_ssl_set_bio (&l.ssl, &l, Line::State::send, Line::State::receive, nullptr);
    l.timeoutMs = 1000;

    for (int waited = 0;;)
    {
        const int r = mbedtls_ssl_handshake (&l.ssl);

        if (r == 0)
            return line;

        if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || stopping || ++waited > 10)
        {
            line->state.reset(); // no goodbye on a line that never started
            return nullptr;
        }
    }
}

int PhoneTls::Line::read (void* to, int most, int timeoutMs)
{
    state->timeoutMs = timeoutMs;
    const int got = mbedtls_ssl_read (&state->ssl, static_cast<unsigned char*> (to), (size_t) most);

    if (got > 0)
        return got;

    return got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE ? 0 : -1;
}

bool PhoneTls::Line::write (const void* from, int size)
{
    const auto* bytes = static_cast<const unsigned char*> (from);

    while (size > 0)
    {
        const int sent = mbedtls_ssl_write (&state->ssl, bytes, (size_t) size);

        if (sent == MBEDTLS_ERR_SSL_WANT_READ || sent == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;

        if (sent <= 0)
            return false;

        bytes += sent;
        size -= sent;
    }

    return true;
}
} // namespace taw
