// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <net_encryption.h>

#include <chainparams.h>
#include <crypto/hkdf_sha256_32.h>
#include <net_message.h>
#include <util.h>

int NetCryptedMessage::Read(const char* pch, unsigned bytes)
{
    if (!m_in_data) {
        // copy data to temporary parsing buffer
        unsigned int remaining = m_encryption_handler->GetAADLen() - m_hdr_pos;
        unsigned int copy_bytes = std::min(remaining, bytes);

        memcpy(&vRecv[m_hdr_pos], pch, copy_bytes);
        m_hdr_pos += copy_bytes;

        // if AAD incomplete, exit
        if (m_hdr_pos < m_encryption_handler->GetAADLen()) {
            return copy_bytes;
        }

        // decrypt the length from the AAD
        if (!m_encryption_handler->GetLength(vRecv, m_message_size)) {
            return -1;
        }

        // check and unset rekey bit
        // the counterparty can signal a post-this-message rekey by setting the
        // most significant bit in the (unencrypted) length
        m_rekey_flag = (m_message_size & (1U << 23));
        if (m_rekey_flag) {
            LogPrint(BCLog::NET, "Rekey flag detected %ld\n", m_message_size);
            m_message_size &= ~(1U << 23);
        }

        // reject messages larger than MAX_SIZE
        if (m_message_size > MAX_SIZE) {
            LogPrint(BCLog::NET, "Max message size exceeded %ld\n", m_message_size);
            return -1;
        }

        // switch state to reading message data
        m_in_data = true;

        return copy_bytes;
    } else {
        // copy the message payload plus the MAC tag
        const unsigned int AAD_LEN = m_encryption_handler->GetAADLen();
        const unsigned int TAG_LEN = m_encryption_handler->GetTagLen();
        unsigned int remaining = m_message_size + TAG_LEN - m_data_pos;
        unsigned int copy_bytes = std::min(remaining, bytes);

        // extend buffer, respect previous copied AAD part
        if (vRecv.size() < AAD_LEN + m_data_pos + copy_bytes) {
            // Allocate up to 256 KiB ahead, but never more than the total message size (incl. AAD & TAG).
            vRecv.resize(std::min(AAD_LEN + m_message_size + TAG_LEN, AAD_LEN + m_data_pos + copy_bytes + 256 * 1024 + TAG_LEN));
        }

        memcpy(&vRecv[AAD_LEN + m_data_pos], pch, copy_bytes);
        m_data_pos += copy_bytes;

        if (Complete()) {
            // authenticate and decrypt if the message is complete
            if (!m_encryption_handler->AuthenticatedAndDecrypt(vRecv)) {
                LogPrint(BCLog::NET, "Authentication or decryption failed\n");
                return false;
            }

            // vRecv holds now the plaintext message excluding the AAD and MAC
            // m_message_size holds the packet size excluding the MAC

            // initially check the message
            try {
                vRecv >> m_command_name;
            } catch (const std::exception&) {
                return false;
            }
            // vRecv points now to the plaintext message payload (MAC is removed)

            if (m_rekey_flag) {
                // post decrypt rekey if rekey was requested
                m_encryption_handler->Rekey(false);
            }
        }
        return copy_bytes;
    }
}

int NetMessageEncryptionHandshake::Read(const char* pch, unsigned int bytes)
{
    // copy data to temporary parsing buffer
    unsigned int remaining = 32 - m_data_pos;
    unsigned int copy_bytes = std::min(remaining, bytes);
    if (vRecv.size() < 32) {
        vRecv.resize(32);
    }
    memcpy(&vRecv[m_data_pos], pch, copy_bytes);
    m_data_pos += copy_bytes;

    return copy_bytes;
}

bool NetMessageEncryptionHandshake::VerifyHeader() const
{
    CMessageHeader hdr(Params().MessageStart());
    CDataStream str = vRecv; //copy stream to keep function const
    try {
        str >> hdr;
    } catch (const std::exception&) {
        return false;
    }
    if (memcmp(hdr.pchMessageStart, Params().MessageStart(), CMessageHeader::MESSAGE_START_SIZE) == 0 || hdr.GetCommand() == NetMsgType::VERSION) {
        return false;
    }
    return true;
}

bool BIP151Encryption::AuthenticatedAndDecrypt(CDataStream& data_in_out)
{
    // create a buffer for the decrypted payload
    std::vector<unsigned char> buf_dec;
    buf_dec.resize(data_in_out.size());

    // keep the original payload size
    size_t vsize = data_in_out.size();

    LOCK(cs);
    if (m_bytes_decrypted + vsize > ABORT_LIMIT_BYTES || GetTime() - m_time_last_rekey_send > ABORT_LIMIT_TIME ||
        (gArgs.GetBoolArg("-netencryptionfastrekey", false) && m_bytes_decrypted + vsize > 12 * 1024)) {
        // don't further decrypt and therefore abort connection when counterparty failed to respect rekey limits
        return false;
    }
    // authenticate and decrypt the message
    if (chacha20poly1305_crypt(&m_recv_aead_ctx, m_recv_seq_nr++, &buf_dec[0], (const uint8_t*)&data_in_out.data()[0],
            data_in_out.size() - TAG_LEN - AAD_LEN, AAD_LEN, 0) == -1) {
        memory_cleanse(data_in_out.data(), data_in_out.size());
        return false;
    }

    // append chacha20 main payload size
    m_bytes_decrypted += data_in_out.size() - TAG_LEN - AAD_LEN;

    data_in_out.clear();
    data_in_out.write((const char*)&buf_dec.begin()[AAD_LEN], vsize - TAG_LEN - AAD_LEN);
    return true;
}

bool BIP151Encryption::EncryptAppendMAC(std::vector<unsigned char>& data_in_out)
{
    // first 3 bytes are the LE uint32 message length the most significant bit
    // indicates to the counterparty that the next message will be using the next
    // key (rekey) with reset nonce
    if (data_in_out[2] & (1u << 7)) {
        // length is only allowed up to 2^23
        return false;
    }
    bool should_rekey = ShouldRekeySend();
    if (should_rekey) {
        // set the rekey flag and signal that the next message will be encrypted
        // with the next key (and reset sequence)
        // rekey flag is the most significant bit encoded in LE (Bitcoin serialization rule)
        data_in_out[2] |= (1u << 7);
    }

    // create a buffer for the encrypted payload
    std::vector<unsigned char> buf_enc;
    buf_enc.resize(data_in_out.size() + TAG_LEN);

    // encrypt and add MAC tag
    LOCK(cs);
    chacha20poly1305_crypt(&m_send_aead_ctx, m_send_seq_nr++, &buf_enc[0], &data_in_out[0],
        data_in_out.size() - AAD_LEN, AAD_LEN, 1);

    // Count total bytes encrypted
    m_bytes_encrypted += data_in_out.size() - AAD_LEN;

    // clear data_in and append the decrypted data
    data_in_out.clear();
    data_in_out.insert(data_in_out.begin(), buf_enc.begin(), buf_enc.end());

    // if it is time to rekey, rekey post encryption
    if (should_rekey) {
        Rekey(true);
    }
    return true;
}

bool BIP151Encryption::GetLength(CDataStream& data_in, uint32_t& len_out)
{
    if (data_in.size() < AAD_LEN) {
        return false;
    }

    LOCK(cs);
    if (chacha20poly1305_get_length24(&m_recv_aead_ctx, &len_out, m_recv_seq_nr, (const uint8_t*)&data_in.data()[0]) == -1) {
        return false;
    }

    return true;
}

bool BIP151Encryption::ShouldCryptMsg()
{
    return handshake_done;
}

uint256 BIP151Encryption::GetSessionID()
{
    LOCK(cs);
    return m_session_id;
}

void BIP151Encryption::EnableEncryption(bool inbound)
{
    LOCK(cs);
    if (m_raw_ecdh_secret.size() != 32) {
        return;
    }
    m_inbound = inbound;
    // extract 2 keys for each direction with HKDF HMAC_SHA256 with length 32
    CHKDF_HMAC_SHA256_L32 hkdf_32(&m_raw_ecdh_secret[0], 32, "BitcoinSharedSecret");
    hkdf_32.Expand32("BitcoinK1A", &m_k1_encryption_keypack[0]);
    hkdf_32.Expand32("BitcoinK1B", &m_k1_encryption_keypack[32]);
    hkdf_32.Expand32("BitcoinK2A", &m_k2_encryption_keypack[0]);
    hkdf_32.Expand32("BitcoinK2B", &m_k2_encryption_keypack[32]);
    hkdf_32.Expand32("BitcoinSessionID", m_session_id.begin());

    m_bytes_encrypted = 0;
    m_time_last_rekey_send = GetTime();
    m_time_last_rekey_recv = m_time_last_rekey_send;

    // enabling k1 for send channel on requesting peer and for recv channel on responding peer
    // enabling k2 for recv channel on requesting peer and for send channel on responding peer
    chacha20poly1305_init(&m_send_aead_ctx, inbound ? m_k2_encryption_keypack.data() : m_k1_encryption_keypack.data(), m_k1_encryption_keypack.size());
    chacha20poly1305_init(&m_recv_aead_ctx, inbound ? m_k1_encryption_keypack.data() : m_k2_encryption_keypack.data(), m_k1_encryption_keypack.size());

    handshake_done = true;
}

bool BIP151Encryption::GetHandshakeRequestData(std::vector<unsigned char>& handshake_data)
{
    LOCK(cs);
    CPubKey pubkey = m_ecdh_key.GetPubKey();
    m_ecdh_key.VerifyPubKey(pubkey); //verify the pubkey
    assert(pubkey[0] == 2);

    handshake_data.insert(handshake_data.begin(), pubkey.begin() + 1, pubkey.end());
    return true;
}

bool BIP151Encryption::ProcessHandshakeRequestData(const std::vector<unsigned char>& handshake_data)
{
    CPubKey pubkey;
    if (handshake_data.size() != 32) {
        return false;
    }
    std::vector<unsigned char> handshake_data_even_pubkey;
    handshake_data_even_pubkey.push_back(2);
    handshake_data_even_pubkey.insert(handshake_data_even_pubkey.begin() + 1, handshake_data.begin(), handshake_data.end());
    pubkey.Set(handshake_data_even_pubkey.begin(), handshake_data_even_pubkey.end());
    if (!pubkey.IsFullyValid()) {
        return false;
    }

    // calculate ECDH secret
    LOCK(cs);
    bool ret = m_ecdh_key.ComputeECDHSecret(pubkey, m_raw_ecdh_secret);

    // After calculating the ECDH secret, the ephemeral key can be cleansed from memory
    m_ecdh_key.SetNull();
    return ret;
}

BIP151Encryption::BIP151Encryption() : handshake_done(false)
{
    m_k1_encryption_keypack.resize(64);
    m_k2_encryption_keypack.resize(64);
    m_ecdh_key.MakeNewKey(true);
    if (m_ecdh_key.GetPubKey()[0] == 3) {
        // the encryption handshake will only use 32byte pubkeys
        // force EVEN (0x02) pubkey be negating the private key in case of ODD (0x03) pubkeys
        m_ecdh_key.Negate();
    }
    assert(m_ecdh_key.IsValid());
}

bool BIP151Encryption::ShouldRekeySend()
{
    LOCK(cs);
    if (!handshake_done) return false;
    int64_t now = GetTime();
    if (gArgs.GetBoolArg("-netencryptionfastrekey", false) &&
        (m_bytes_encrypted >= 12 * 1024 || (now - m_time_last_rekey_send > 10))) {
        // use insane small rekey trigger during re-key tests
        LogPrint(BCLog::NET, "Should rekey (insane -netencryptionfastrekey trigger)\n");
        return true;
    }
    if (m_bytes_encrypted >= REKEY_LIMIT_BYTES || now - m_time_last_rekey_send >= REKEY_LIMIT_TIME) {
        LogPrint(BCLog::NET, "Rekey limits reached\n");
        return true;
    }
    return false;
}

bool BIP151Encryption::Rekey(bool send_channel)
{
    LOCK(cs);
    int64_t now = GetTime();
    if (!send_channel && now - m_time_last_rekey_recv < MIN_REKEY_TIME) {
        // requested rekey was below the minimal rekey time: reject
        LogPrint(BCLog::NET, "Reject rekey (DOS limits)\n");
        return false;
    }
    LogPrint(BCLog::NET, "Rekey %s channel\n", send_channel ? "send" : "recv");
    CPrivKey* keypack;
    if (send_channel) {
        keypack = m_inbound ? &m_k2_encryption_keypack : &m_k1_encryption_keypack;
    }
    else {
        keypack = m_inbound ? &m_k1_encryption_keypack : &m_k2_encryption_keypack;
    }
    // rekey after BIP151 rules SHA256(SHA256(session_id || old_symmetric_cipher_key))
    // rekey for both keys (AAD and payload key)
    uint256 new_k0 = Hash(m_session_id.begin(), m_session_id.end(), keypack->begin(), keypack->begin() + 32);
    uint256 new_k1 = Hash(m_session_id.begin(), m_session_id.end(), keypack->begin() + 32, keypack->end());

    // replace keys
    keypack->clear();
    keypack->insert(keypack->begin(), new_k0.begin(), new_k0.end());
    keypack->insert(keypack->end(), new_k1.begin(), new_k1.end());

    // reset byte and time counter
    if (send_channel) {
        m_bytes_encrypted = 0;
        m_time_last_rekey_send = GetTime();
        chacha20poly1305_init(&m_send_aead_ctx, keypack->data(), keypack->size());
    } else {
        m_bytes_decrypted = 0;
        m_time_last_rekey_recv = GetTime();
        chacha20poly1305_init(&m_recv_aead_ctx, keypack->data(), keypack->size());
    }
    return true;
}
