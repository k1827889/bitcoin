// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"

#include <stdint.h>

#include "corewallet/corewallet_db.h"
#include "corewallet/corewallet_wallet.h"

namespace CoreWallet
{
    
bool FileDB::WriteKey(const CPubKey& vchPubKey, const CPrivKey& vchPrivKey, const CKeyMetadata& keyMeta)
{
    if (!Write(std::make_pair(std::string("keymeta"), vchPubKey), keyMeta, false))
        return false;
    
    // hash pubkey/privkey to accelerate wallet load
    std::vector<unsigned char> vchKey;
    vchKey.reserve(vchPubKey.size() + vchPrivKey.size());
    vchKey.insert(vchKey.end(), vchPubKey.begin(), vchPubKey.end());
    vchKey.insert(vchKey.end(), vchPrivKey.begin(), vchPrivKey.end());
    
    return Write(std::make_pair(std::string("key"), vchPubKey), std::make_pair(vchPrivKey, Hash(vchKey.begin(), vchKey.end())), false);
}

bool ReadKeyValue(Wallet* pCoreWallet, CDataStream& ssKey, CDataStream& ssValue, std::string& strType, std::string& strErr)
{
    try {
        ssKey >> strType;
        if (strType == "key")
        {
            CPubKey vchPubKey;
            ssKey >> vchPubKey;
            if (!vchPubKey.IsValid())
            {
                strErr = "Error reading wallet database: CPubKey corrupt";
                return false;
            }
            CKey key;
            CPrivKey pkey;
            uint256 hash;
            
            ssValue >> pkey;
            ssValue >> hash;

            bool fSkipCheck = false;
            
            if (!hash.IsNull())
            {
                // hash pubkey/privkey to accelerate wallet load
                std::vector<unsigned char> vchKey;
                vchKey.reserve(vchPubKey.size() + pkey.size());
                vchKey.insert(vchKey.end(), vchPubKey.begin(), vchPubKey.end());
                vchKey.insert(vchKey.end(), pkey.begin(), pkey.end());
                
                if (Hash(vchKey.begin(), vchKey.end()) != hash)
                {
                    strErr = "Error reading wallet database: CPubKey/CPrivKey corrupt";
                    return false;
                }
                
                fSkipCheck = true;
            }
            
            if (!key.Load(pkey, vchPubKey, fSkipCheck))
            {
                strErr = "Error reading wallet database: CPrivKey corrupt";
                return false;
            }
            if (!pCoreWallet->LoadKey(key, vchPubKey))
            {
                strErr = "Error reading wallet database: LoadKey failed";
                return false;
            }
        }
        else if (strType == "keymeta")
        {
            CPubKey vchPubKey;
            ssKey >> vchPubKey;
            CKeyMetadata keyMeta;
            ssValue >> keyMeta;
            
            pCoreWallet->LoadKeyMetadata(vchPubKey, keyMeta);
            
            // find earliest key creation time, as wallet birthday
            if (!pCoreWallet->nTimeFirstKey ||
                (keyMeta.nCreateTime < pCoreWallet->nTimeFirstKey))
                pCoreWallet->nTimeFirstKey = keyMeta.nCreateTime;
        }
        else if (strType == "adrmeta")
        {
            std::string strAddress;
            CAddressBookMetadata metadata;
            ssKey >> strAddress;
            ssValue >> metadata;
            pCoreWallet->mapAddressBook[CBitcoinAddress(strAddress).Get()] = metadata;
        }
        else if (strType == "masterkeyid")
        {
            ssValue >> pCoreWallet->masterKeyID;
        }
        else if (strType == "bip32intpubkey")
        {
            ssValue >> pCoreWallet->internalPubKey.pubkey;
        }
        else if (strType == "bip32extpubkey")
        {
            ssValue >> pCoreWallet->externalPubKey.pubkey;
        }
        else if (strType == "chainpath")
        {
            ssValue >> pCoreWallet->strChainPath;
        }
        else if (strType == "masterseed")
        {
            uint32_t seedNum;

            ssKey >> seedNum;
            ssValue >> pCoreWallet->strMasterseedHex;
        }
        else if (strType == "internalpubkey")
        {
            ssValue >> pCoreWallet->internalPubKey;
        }
        else if (strType == "externalpubkey")
        {
            ssValue >> pCoreWallet->externalPubKey;
        }
        else if (strType == "extpubkey")
        {
            CKeyID keyId;
            CExtPubKey extPubKey;

            ssKey >> keyId;
            ssValue >> extPubKey;
        }



    } catch (...)
    {
        return false;
    }
    return true;
}

bool FileDB::LoadWallet(Wallet* pCoreWallet)
{
    if(!Load())
        return false;
    
    bool fNoncriticalErrors = false;
    bool result = true;
    
    bool fAutoTransaction = TxnBegin();
    
    try {
        LOCK(pCoreWallet->cs_coreWallet);
        for (FileDB::const_iterator it = begin(); it != end(); it++)
        {
            // Read next record
            CDataStream ssKey((*it).first, SER_DISK, CLIENT_VERSION);
            CDataStream ssValue((*it).second, SER_DISK, CLIENT_VERSION);
            
            // Try to be tolerant of single corrupt records:
            std::string strType, strErr;
            if (!ReadKeyValue(pCoreWallet, ssKey, ssValue, strType, strErr))
            {
                // losing keys is considered a catastrophic error, anything else
                // we assume the user can live with:
                if (strType == "key")
                    result = false;
                else
                {
                    // Leave other errors alone, if we try to fix them we might make things worse.
                    fNoncriticalErrors = true; // ... but do warn the user there is something wrong.
                    if (strType == "tx")
                        // Rescan if there is a bad transaction record:
                        SoftSetBoolArg("-rescan", true);
                }
            }
            if (!strErr.empty())
                LogPrintf("%s\n", strErr);
        }
    }
    catch (const boost::thread_interrupted&) {
        throw;
    }
    catch (...) {
        result = false;
    }
    
    if (fNoncriticalErrors && result == true)
        result = false;
    
    // Any wallet corruption at all: skip any rewriting or
    // upgrading, we don't want to make it worse.
    if (result != true)
        return result;
    
    if (fAutoTransaction)
        TxnCommit();
    
    return result;
}

}; //end namespace