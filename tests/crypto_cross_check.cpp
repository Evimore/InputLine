// Cross-checks InputLine's own crypto in core/ against libsodium on thousands
// of random inputs: X25519, ChaCha20, Poly1305, ChaCha20-Poly1305, SHA-256,
// HMAC-SHA-256 and SHA-512. A test only: InputLine itself never uses
// libsodium. Built with -DINPUTLINE_CROSS_CHECK=ON (CI does).

#include "inputline/crypto.h"
#include "inputline/sha256.h"
#include "inputline/sha512.h"

#include <sodium.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace inputline;
using namespace inputline::crypto;

namespace {

  int g_checks = 0;
  int g_failures = 0;

#define CHECK(condition) \
  do { \
    ++g_checks; \
    if (!(condition)) { \
      ++g_failures; \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
    } \
  } while (0)

  // A fixed seed, so a failure can be reproduced.
  std::mt19937_64 g_random(20261008);

  std::size_t below(std::size_t limit) {
    return static_cast<std::size_t>(g_random() % limit);
  }

  /** Random bytes, one spare so data() is never null. */
  std::vector<std::uint8_t> random_bytes(std::size_t length) {
    std::vector<std::uint8_t> out(length + 1);
    for (auto &byte : out) {
      byte = static_cast<std::uint8_t>(g_random());
    }
    return out;
  }

  template<std::size_t N>
  std::array<std::uint8_t, N> random_array() {
    std::array<std::uint8_t, N> out {};
    for (auto &byte : out) {
      byte = static_cast<std::uint8_t>(g_random());
    }
    return out;
  }

  void check_x25519() {
    for (int i = 0; i < 300; ++i) {  // the slowest: fewer rounds keep the sanitizer build quick
      const auto secret = random_array<32>();
      std::uint8_t expected[32];
      CHECK(crypto_scalarmult_curve25519_base(expected, secret.data()) == 0);
      CHECK(std::memcmp(x25519_public_key(secret).data(), expected, 32) == 0);

      // Any 32 bytes as the peer's key: on the curve or its twist, bit 255 set or not.
      const auto peer = random_array<32>();
      Key32 shared {};
      const bool ours = x25519(secret, peer, shared);
      std::uint8_t theirs[32];
      const bool sodium = crypto_scalarmult_curve25519(theirs, secret.data(), peer.data()) == 0;
      CHECK(ours == sodium);
      if (ours && sodium) {
        CHECK(std::memcmp(shared.data(), theirs, 32) == 0);
      }
    }

    // Small-order keys, including non-canonical encodings: both refuse them.
    const char *const small_order[] = {
      "0000000000000000000000000000000000000000000000000000000000000000",
      "0100000000000000000000000000000000000000000000000000000000000000",
      "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
      "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
      "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
      "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
      "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    };
    for (const char *text : small_order) {
      Key32 peer {};
      for (std::size_t i = 0; i < 32; ++i) {
        peer[i] = static_cast<std::uint8_t>(std::stoul(std::string(text + 2 * i, 2), nullptr, 16));
      }
      const auto secret = random_array<32>();
      Key32 shared {};
      std::uint8_t theirs[32];
      CHECK(!x25519(secret, peer, shared));
      CHECK(crypto_scalarmult_curve25519(theirs, secret.data(), peer.data()) != 0);
    }
  }

  void check_chacha20() {
    for (int i = 0; i < 1000; ++i) {
      const auto key = random_array<32>();
      const auto nonce = random_array<12>();
      const auto counter = static_cast<std::uint32_t>(below(1u << 31));
      const std::size_t length = below(300);
      const auto input = random_bytes(length);
      std::vector<std::uint8_t> ours(length + 1), theirs(length + 1);
      chacha20_xor(key, counter, nonce, input.data(), ours.data(), length);
      CHECK(crypto_stream_chacha20_ietf_xor_ic(theirs.data(), input.data(), length, nonce.data(), counter, key.data()) == 0);
      CHECK(std::memcmp(ours.data(), theirs.data(), length) == 0);
    }
  }

  void check_poly1305() {
    for (int i = 0; i < 1000; ++i) {
      const auto key = random_array<32>();
      const std::size_t length = below(300);
      const auto message = random_bytes(length);
      std::uint8_t theirs[16];
      CHECK(crypto_onetimeauth_poly1305(theirs, message.data(), length, key.data()) == 0);
      CHECK(std::memcmp(poly1305(key, message.data(), length).data(), theirs, 16) == 0);
    }
  }

  void check_aead() {
    for (int i = 0; i < 2000; ++i) {
      const auto key = random_array<32>();
      const auto nonce = random_array<12>();
      const std::size_t aad_length = below(65);
      const std::size_t length = below(300);
      const auto aad = random_bytes(aad_length);
      const auto plaintext = random_bytes(length);

      std::vector<std::uint8_t> ours(length + 1);
      const Tag16 tag = aead_seal(key, nonce, aad.data(), aad_length, plaintext.data(), ours.data(), length);
      std::vector<std::uint8_t> theirs(length + 1);
      std::uint8_t their_tag[16];
      unsigned long long tag_length = 0;
      CHECK(crypto_aead_chacha20poly1305_ietf_encrypt_detached(theirs.data(), their_tag, &tag_length, plaintext.data(), length, aad.data(),
                                                                aad_length, nullptr, nonce.data(), key.data()) == 0);
      CHECK(tag_length == 16);
      CHECK(std::memcmp(ours.data(), theirs.data(), length) == 0);
      CHECK(std::memcmp(tag.data(), their_tag, 16) == 0);

      // Each opens what the other sealed.
      std::vector<std::uint8_t> opened(length + 1);
      Tag16 sodium_tag {};
      std::memcpy(sodium_tag.data(), their_tag, 16);
      CHECK(aead_open(key, nonce, aad.data(), aad_length, theirs.data(), opened.data(), length, sodium_tag));
      CHECK(std::memcmp(opened.data(), plaintext.data(), length) == 0);
      CHECK(crypto_aead_chacha20poly1305_ietf_decrypt_detached(opened.data(), nullptr, ours.data(), length, tag.data(), aad.data(), aad_length,
                                                                nonce.data(), key.data()) == 0);

      // Both refuse one flipped bit, in the ciphertext or the tag.
      Tag16 bad_tag = tag;
      std::vector<std::uint8_t> bad = ours;
      if (length > 0 && below(2) == 0) {
        bad[below(length)] ^= static_cast<std::uint8_t>(1u << below(8));
      } else {
        bad_tag[below(16)] ^= static_cast<std::uint8_t>(1u << below(8));
      }
      CHECK(!aead_open(key, nonce, aad.data(), aad_length, bad.data(), opened.data(), length, bad_tag));
      CHECK(crypto_aead_chacha20poly1305_ietf_decrypt_detached(opened.data(), nullptr, bad.data(), length, bad_tag.data(), aad.data(),
                                                                aad_length, nonce.data(), key.data()) != 0);
    }
  }

  void check_hashes() {
    for (int i = 0; i < 1000; ++i) {
      const std::size_t length = below(600);
      const auto data = random_bytes(length);

      std::uint8_t sha256[32];
      crypto_hash_sha256(sha256, data.data(), length);
      CHECK(std::memcmp(Sha256::hash(data.data(), length).data(), sha256, 32) == 0);
      std::uint8_t sha512[64];
      crypto_hash_sha512(sha512, data.data(), length);
      CHECK(std::memcmp(Sha512::hash(data.data(), length).data(), sha512, 64) == 0);

      // The same, fed in random pieces.
      Sha256 pieces256;
      Sha512 pieces512;
      for (std::size_t done = 0; done < length;) {
        const std::size_t piece = 1 + below(length - done);
        pieces256.update(data.data() + done, piece);
        pieces512.update(data.data() + done, piece);
        done += piece;
      }
      CHECK(std::memcmp(pieces256.finish().data(), sha256, 32) == 0);
      CHECK(std::memcmp(pieces512.finish().data(), sha512, 64) == 0);

      // HMAC-SHA-256 with keys shorter and longer than a block.
      const std::size_t key_length = below(200);
      const auto key = random_bytes(key_length);
      crypto_auth_hmacsha256_state state;
      std::uint8_t mac[32];
      crypto_auth_hmacsha256_init(&state, key.data(), key_length);
      crypto_auth_hmacsha256_update(&state, data.data(), length);
      crypto_auth_hmacsha256_final(&state, mac);
      CHECK(std::memcmp(HmacSha256::mac(key.data(), key_length, data.data(), length).data(), mac, 32) == 0);
    }
  }

}  // namespace

int main() {
  if (sodium_init() < 0) {
    std::fprintf(stderr, "libsodium failed to start\n");
    return 1;
  }
  check_x25519();
  check_chacha20();
  check_poly1305();
  check_aead();
  check_hashes();
  std::printf("%d checks against libsodium, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
