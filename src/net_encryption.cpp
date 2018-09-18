// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <net_encryption.h>
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

        // reject messages larger than MAX_SIZE
        if (m_message_size > MAX_SIZE) {
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
        }

        return copy_bytes;
    }
}

bool BIP151Encryption::AuthenticatedAndDecrypt(CDataStream& data_in_out)
{
    // create a buffer for the decrypted payload
    std::vector<unsigned char> buf_dec;
    buf_dec.resize(data_in_out.size());

    // keep the original payload size
    size_t vsize = data_in_out.size();

    // authenticate and decrypt the message
    LOCK(cs);
    if (chacha20poly1305_crypt(&m_aead_ctx, m_recv_seq_nr++, &buf_dec[0], (const uint8_t*)&data_in_out.data()[0],
            data_in_out.size() - TAG_LEN - AAD_LEN, AAD_LEN, 0) == -1) {
        memory_cleanse(data_in_out.data(), data_in_out.size());
        return false;
    }

    data_in_out.clear();
    data_in_out.write((const char*)&buf_dec.begin()[AAD_LEN], vsize - TAG_LEN - AAD_LEN);
    return true;
}

bool BIP151Encryption::EncryptAppendMAC(std::vector<unsigned char>& data_in_out)
{
    // create a buffer for the encrypted payload
    std::vector<unsigned char> buf_enc;
    buf_enc.resize(data_in_out.size() + TAG_LEN);

    // encrypt and add MAC tag
    LOCK(cs);
    chacha20poly1305_crypt(&m_aead_ctx, m_send_seq_nr++, &buf_enc[0], &data_in_out[0],
        data_in_out.size() - AAD_LEN, AAD_LEN, 1);

    // clear data_in and append the decrypted data
    data_in_out.clear();
    data_in_out.insert(data_in_out.begin(), buf_enc.begin(), buf_enc.end());
    return true;
}

bool BIP151Encryption::GetLength(CDataStream& data_in, uint32_t& len_out)
{
    if (data_in.size() < AAD_LEN) {
        return false;
    }

    LOCK(cs);
    if (chacha20poly1305_get_length24(&m_aead_ctx, &len_out, m_recv_seq_nr, (const uint8_t*)&data_in.data()[0]) == -1) {
        return false;
    }

    return true;
}

bool BIP151Encryption::ShouldCryptMsg()
{
    return true;
}

BIP151Encryption::BIP151Encryption()
{
    uint8_t aead_keys[64] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
        0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    chacha20poly1305_init(&m_aead_ctx, aead_keys, sizeof(aead_keys));
}
