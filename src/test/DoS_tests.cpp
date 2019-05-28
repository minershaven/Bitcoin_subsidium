// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017 The BitcoinSubsidium Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for denial-of-service detection/prevention code

#include "chainparams.h"
#include "keystore.h"
#include "net.h"
#include "net_processing.h"
#include "pow.h"
#include "script/sign.h"
#include "serialize.h"
#include "util.h"
#include "validation.h"

#include "test/test_BitcoinSubsidium.h"

#include <stdint.h>

#include <boost/test/unit_test.hpp>

// Tests these internal-to-net_processing.cpp methods:
extern bool AddOrphanTx(const CTransactionRef &tx, NodeId peer);

extern void EraseOrphansFor(NodeId peer);

extern unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans);

struct COrphanTx
{
    CTransactionRef tx;
    NodeId fromPeer;
    int64_t nTimeExpire;
};
extern std::map<uint256, COrphanTx> mapOrphanTransactions;

CService ip(uint32_t i)
{
    struct in_addr s;
    s.s_addr = i;
    return CService(CNetAddr(s), Params().GetDefaultPort());
}

static NodeId id = 0;

BOOST_FIXTURE_TEST_SUITE(DoS_tests, TestingSetup)

// Test eviction of an outbound peer whose chain never advances
// Mock a node connection, and use mocktime to simulate a peer
// which never sends any headers messages.  PeerLogic should
// decide to evict that outbound peer, after the appropriate timeouts.
// Note that we protect 4 outbound nodes from being subject to
// this logic; this test takes advantage of that protection only
// being applied to nodes which send headers with sufficient
// work.
    BOOST_AUTO_TEST_CASE(outbound_slow_chain_eviction_test)
    {
        BOOST_TEST_MESSAGE("Running Outbound Slow Chain Eviction Test");

        std::atomic<bool> interruptDummy(false);

        // Mock an outbound peer
        CAddress addr1(ip(0xa0b0c001), NODE_NONE);
        CNode dummyNode1(id++, ServiceFlags(NODE_NETWORK | NODE_WITNESS), 0, INVALID_SOCKET, addr1, 0, 0, CAddress(), "", /*fInboundIn=*/ false);
        dummyNode1.SetSendVersion(PROTOCOL_VERSION);

        peerLogic->InitializeNode(&dummyNode1);
        dummyNode1.nVersion = 1;
        dummyNode1.fSuccessfullyConnected = true;

        // This test requires that we have a chain with non-zero work.
        BOOST_CHECK(chainActive.Tip() != nullptr);
        BOOST_CHECK(chainActive.Tip()->nChainWork > 0);

        // Test starts here
        peerLogic->SendMessages(&dummyNode1, interruptDummy); // should result in getheaders
        BOOST_CHECK(dummyNode1.vSendMsg.size() > 0);
        dummyNode1.vSendMsg.clear();

        int64_t nStartTime = GetTime();
        // Wait 21 minutes
        SetMockTime(nStartTime + 21 * 60);
        peerLogic->SendMessages(&dummyNode1, interruptDummy); // should result in getheaders
        BOOST_CHECK(dummyNode1.vSendMsg.size() > 0);
        // Wait 3 more minutes
        SetMockTime(nStartTime + 24 * 60);
        peerLogic->SendMessages(&dummyNode1, interruptDummy); // should result in disconnect
        BOOST_CHECK(dummyNode1.fDisconnect == true);
        SetMockTime(0);

        bool dummy;
        peerLogic->FinalizeNode(dummyNode1.GetId(), dummy);
    }

    BOOST_AUTO_TEST_CASE(DoS_banning_test)
    {
        BOOST_TEST_MESSAGE("Running DoS Banning Test");

        std::atomic<bool> interruptDummy(false);

        connman->ClearBanned();
        CAddress addr1(ip(0xa0b0c001), NODE_NONE);
        CNode dummyNode1(id++, NODE_NETWORK, 0, INVALID_SOCKET, addr1, 0, 0, CAddress(), "", true);
        dummyNode1.SetSendVersion(PROTOCOL_VERSION);
        peerLogic->InitializeNode(&dummyNode1);
        dummyNode1.nVersion = 1;
        dummyNode1.fSuccessfullyConnected = true;
        Misbehaving(dummyNode1.GetId(), 100); // Should get banned
        peerLogic->SendMessages(&dummyNode1, interruptDummy);
        BOOST_CHECK(connman->IsBanned(addr1));
        BOOST_CHECK(!connman->IsBanned(ip(0xa0b0c001 | 0x0000ff00))); // Different IP, not banned

        CAddress addr2(ip(0xa0b0c002), NODE_NONE);
        CNode dummyNode2(id++, NODE_NETWORK, 0, INVALID_SOCKET, addr2, 1, 1, CAddress(), "", true);
        dummyNode2.SetSendVersion(PROTOCOL_VERSION);
        peerLogic->InitializeNode(&dummyNode2);
        dummyNode2.nVersion = 1;
        dummyNode2.fSuccessfullyConnected = true;
        Misbehaving(dummyNode2.GetId(), 50);
        peerLogic->SendMessages(&dummyNode2, interruptDummy);
        BOOST_CHECK(!connman->IsBanned(addr2)); // 2 not banned yet...
        BOOST_CHECK(connman->IsBanned(addr1));  // ... but 1 still should be
        Misbehaving(dummyNode2.GetId(), 50);
        peerLogic->SendMessages(&dummyNode2, interruptDummy);
        BOOST_CHECK(connman->IsBanned(addr2));

        bool dummy;
        peerLogic->FinalizeNode(dummyNode1.GetId(), dummy);
        peerLogic->FinalizeNode(dummyNode2.GetId(), dummy);
    }

    BOOST_AUTO_TEST_CASE(DoS_banscore_test)
    {
        BOOST_TEST_MESSAGE("Running DoS Banscore Test Test");

        std::atomic<bool> interruptDummy(false);

        connman->ClearBanned();
        gArgs.ForceSetArg("-banscore", "111"); // because 11 is my favorite number
        CAddress addr1(ip(0xa0b0c001), NODE_NONE);
        CNode dummyNode1(id++, NODE_NETWORK, 0, INVALID_SOCKET, addr1, 3, 1, CAddress(), "", true);
        dummyNode1.SetSendVersion(PROTOCOL_VERSION);
        peerLogic->InitializeNode(&dummyNode1);
        dummyNode1.nVersion = 1;
        dummyNode1.fSuccessfullyConnected = true;
        Misbehaving(dummyNode1.GetId(), 100);
        peerLogic->SendMessages(&dummyNode1, interruptDummy);
        BOOST_CHECK(!connman->IsBanned(addr1));
        Misbehaving(dummyNode1.GetId(), 10);
        peerLogic->SendMessages(&dummyNode1, interruptDummy);
        BOOST_CHECK(!connman->IsBanned(addr1));
        Misbehaving(dummyNode1.GetId(), 1);
        peerLogic->SendMessages(&dummyNode1, interruptDummy);
        BOOST_CHECK(connman->IsBanned(addr1));
        gArgs.ForceSetArg("-banscore", std::to_string(DEFAULT_BANSCORE_THRESHOLD));

        bool dummy;
        peerLogic->FinalizeNode(dummyNode1.GetId(), dummy);
    }

    BOOST_AUTO_TEST_CASE(DoS_bantime_test)
    {
        BOOST_TEST_MESSAGE("Running DoS Bantime Test");

        std::atomic<bool> interruptDummy(false);

        connman->ClearBanned();
        int64_t nStartTime = GetTime();
        SetMockTime(nStartTime); // Overrides future calls to GetTime()

        CAddress addr(ip(0xa0b0c001), NODE_NONE);
        CNode dummyNode(id++, NODE_NETWORK, 0, INVALID_SOCKET, addr, 4, 4, CAddress(), "", true);
        dummyNode.SetSendVersion(PROTOCOL_VERSION);
        peerLogic->InitializeNode(&dummyNode);
        dummyNode.nVersion = 1;
        dummyNode.fSuccessfullyConnected = true;

        Misbehaving(dummyNode.GetId(), 100);
        peerLogic->SendMessages(&dummyNode, interruptDummy);
        BOOST_CHECK(connman->IsBanned(addr));

        SetMockTime(nStartTime + 60 * 60);
        BOOST_CHECK(connman->IsBanned(addr));

        SetMockTime(nStartTime + 60 * 60 * 24 + 1);
        BOOST_CHECK(!connman->IsBanned(addr));

        bool dummy;
        peerLogic->FinalizeNode(dummyNode.GetId(), dummy);
    }

    CTransactionRef RandomOrphan()
    {
        std::map<uint256, COrphanTx>::iterator it;
        it = mapOrphanTransactions.lower_bound(InsecureRand256());
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        return it->second.tx;
    }

    BOOST_AUTO_TEST_CASE(DoS_maporphans_test)
    {
        BOOST_TEST_MESSAGE("Running DoS MapOrphans Test");

        CKey key;
        key.MakeNewKey(true);
        CBasicKeyStore keystore;
        keystore.AddKey(key);

        // 50 orphan transactions:
        for (int i = 0; i < 50; i++)
        {
            CMutableTransaction tx;
            tx.vin.resize(1);
            tx.vin[0].prevout.n = 0;
            tx.vin[0].prevout.hash = InsecureRand256();
            tx.vin[0].scriptSig << OP_1;
            tx.vout.resize(1);
            tx.vout[0].nValue = 1 * CENT;
            tx.vout[0].scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());

            AddOrphanTx(MakeTransactionRef(tx), i);
        }

        // ... and 50 that depend on other orphans:
        for (int i = 0; i < 50; i++)
        {
            CTransactionRef txPrev = RandomOrphan();

            CMutableTransaction tx;
            tx.vin.resize(1);
            tx.vin[0].prevout.n = 0;
            tx.vin[0].prevout.hash = txPrev->GetHash();
            tx.vout.resize(1);
            tx.vout[0].nValue = 1 * CENT;
            tx.vout[0].scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
            SignSignature(keystore, *txPrev, tx, 0, SIGHASH_ALL);

            AddOrphanTx(MakeTransactionRef(tx), i);
        }

        // This really-big orphan should be ignored:
        for (int i = 0; i < 10; i++)
        {
            CTransactionRef txPrev = RandomOrphan();

            CMutableTransaction tx;
            tx.vout.resize(1);
            tx.vout[0].nValue = 1 * CENT;
            tx.vout[0].scriptPubKey = GetScriptForDestination(key.GetPubKey().GetID());
            tx.vin.resize(2777);
            for (unsigned int j = 0; j < tx.vin.size(); j++)
            {
                tx.vin[j].prevout.n = j;
                tx.vin[j].prevout.hash = txPrev->GetHash();
            }
            SignSignature(keystore, *txPrev, tx, 0, SIGHASH_ALL);
            // Re-use same signature for other inputs
            // (they don't have to be valid for this test)
            for (unsigned int j = 1; j < tx.vin.size(); j++)
                tx.vin[j].scriptSig = tx.vin[0].scriptSig;

            BOOST_CHECK(!AddOrphanTx(MakeTransactionRef(tx), i));
        }

        // Test EraseOrphansFor:
        for (NodeId i = 0; i < 3; i++)
        {
            size_t sizeBefore = mapOrphanTransactions.size();
            EraseOrphansFor(i);
            BOOST_CHECK(mapOrphanTransactions.size() < sizeBefore);
        }

        // Test LimitOrphanTxSize() function:
        LimitOrphanTxSize(40);
        BOOST_CHECK(mapOrphanTransactions.size() <= 40);
        LimitOrphanTxSize(10);
        BOOST_CHECK(mapOrphanTransactions.size() <= 10);
        LimitOrphanTxSize(0);
        BOOST_CHECK(mapOrphanTransactions.empty());
    }

BOOST_AUTO_TEST_SUITE_END()
