// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * vr/replica.cc:
 *   Viewstamped Replication protocol
 *
 * Copyright 2013-2016 Dan R. K. Ports  <drkp@cs.washington.edu>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/

#include "common/replica.h"
#include "vr/replica.h"
#include "vr/vr-proto.pb.h"

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/latency.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "lib/udptransport.h"

#include <algorithm>
#include <random>
#include <string>

#ifdef __cplusplus
extern "C"{
#endif 

#include <asm-generic/posix_types.h>
#include <linux/if_link.h>
#include <linux/limits.h>

#include <linux/bpf.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp-handler/fast_common.h>

#ifdef __cplusplus
}
#endif

// #define FAST_QUORUM_PRUNE
// #define FAST_BATCH // unused!
// #define FAST_REPLY
// #define TC_BROADCAST


#define RDebug(fmt, ...) Debug("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, myIdx, ##__VA_ARGS__)

namespace specpaxos {
namespace vr {

using namespace proto;

#ifdef FAST_BATCH
int request_polling(void *ctx, void *data, size_t data_sz) {
    VRReplica *pt = (VRReplica *)ctx;

    sockaddr_in sender;
    sender.sin_family = AF_INET;
    sender.sin_port = *(in_port_t *)data;
    data += sizeof(in_port_t);
    sender.sin_addr.s_addr = *(in_addr_t *)data;
    data += sizeof(in_addr_t);

    uint64_t size = *(uint64_t *)data;
    static RequestMessage request;
    request.ParseFromString(string((char *)data + sizeof(uint64_t), size));

    UDPTransportAddress senderAddr(sender);
    pt -> HandleRequest(senderAddr, request);
    return 0;
}
#endif

#ifdef FAST_REPLY
int prepare_polling(void *ctx, void *data, size_t data_sz) {
    VRReplica *pt = (VRReplica *)ctx;
    uint64_t size = *(uint64_t *)data;
    static PrepareMessage prepare;
    prepare.ParseFromString(string((char *)data + sizeof(uint64_t), size));

    pt -> HandlePrepare_Kernel(prepare);
    return 0;
}
#endif

VRReplica::VRReplica(Configuration config, int myIdx,
                     bool initialize,
                     Transport *transport, int batchSize,
                     AppReplica *app)
    : Replica(config, myIdx, initialize, transport, app),
      batchSize(batchSize),
      log(false),
      prepareOKQuorum(config.QuorumSize()-1),  // set the quorum.
      startViewChangeQuorum(config.QuorumSize()-1),
      doViewChangeQuorum(config.QuorumSize()-1), // quorum of view change, basically the same.
      recoveryResponseQuorum(config.QuorumSize()) {
    this->status = STATUS_NORMAL; // State is normal.
    this->view = 0;
    this->lastOp = 0;
    this->lastCommitted = 0;
    this->lastRequestStateTransferView = 0;
    this->lastRequestStateTransferOpnum = 0;
    lastBatchEnd = 0;
    batchComplete = true;

    if (batchSize > 1) {
        Notice("Batching enabled; batch size %d", batchSize);
    }

    // If we don't hear from leader for a period of time, then do the view change.
    this->viewChangeTimeout = new Timeout(transport, 5000, [this,myIdx]() {
            RWarning("Have not heard from leader; starting view change");
            StartViewChange(view+1);
        });
    this->nullCommitTimeout = new Timeout(transport, 1000, [this]() {
            SendNullCommit();
        });
    this->stateTransferTimeout = new Timeout(transport, 1000, [this]() {
            this->lastRequestStateTransferView = 0;
            this->lastRequestStateTransferOpnum = 0;            
        });
    this->stateTransferTimeout->Start();
    this->resendPrepareTimeout = new Timeout(transport, 500, [this]() {
            ResendPrepare();
        });
    this->closeBatchTimeout = new Timeout(transport, 300, [this]() {
            #ifdef FAST_BATCH
            ring_buffer__poll(rb_request, 0);
            #endif
            CloseBatch();
        });
    this->recoveryTimeout = new Timeout(transport, 5000, [this]() {
            SendRecoveryMessages();
        });

    _Latency_Init(&requestLatency, "request");
    _Latency_Init(&executeAndReplyLatency, "executeAndReply");

    if (initialize) {
        if (AmLeader()) {
            nullCommitTimeout->Start();  // start heart-beating message.
        } else {
            viewChangeTimeout->Start();
        }
    } else { // recovery... this should be rare case.
        this->status = STATUS_RECOVERING;
        this->recoveryNonce = GenerateNonce();
        SendRecoveryMessages();
        recoveryTimeout->Start();
    }

    memset(sgn_bits, 0, sizeof(sgn_bits));
    sgn_bits[0] = (1<<31); // #define BROADCAST_SIGN_BIT (1<<31)
#ifdef FAST_BATCH
    request_buffer_fd = bpf_obj_get("/sys/fs/bpf/paxos_request_buffer");
    if (request_buffer_fd < 0) {
		fprintf(stderr, "Error: bpf_object__find_map_fd_by_name \"paxos_request_buffer\" failed\n");
		exit(1); //return 1;
	}
    rb_request = ring_buffer__new(request_buffer_fd, request_polling, this, NULL);
	if (!rb_request) {
		fprintf(stderr, "Failed to create request ring buffer\n");
        exit(-1);
	}
#endif
#ifdef FAST_REPLY
    prepare_buffer_fd = bpf_obj_get("/sys/fs/bpf/paxos_prepare_buffer");
    if (prepare_buffer_fd < 0) {
		fprintf(stderr, "Error: bpf_object__find_map_fd_by_name \"paxos_prepare_buffer\" failed\n");
		exit(1); //return 1;
	}
    rb_prepare = ring_buffer__new(prepare_buffer_fd, prepare_polling, this, NULL);
	if (!rb_prepare) {
		fprintf(stderr, "Failed to create prepare ring buffer\n");
        exit(-1);
	}
#endif

#if defined FAST_REPLY || defined FAST_BATCH || defined FAST_QUORUM_PRUNE
    paxos_ctr_state_fd = bpf_obj_get("/sys/fs/bpf/paxos_ctr_state");
    if (paxos_ctr_state_fd < 0) {
		fprintf(stderr, "Error: bpf_object__find_map_fd_by_name \"paxos_ctr_state\" failed\n");
		exit(1); //return 1;
	}
    ModifyKernelState();
#endif
}

VRReplica::~VRReplica() {
    Latency_Dump(&requestLatency);
    Latency_Dump(&executeAndReplyLatency);

    delete viewChangeTimeout;
    delete nullCommitTimeout;
    delete stateTransferTimeout;
    delete resendPrepareTimeout;
    delete closeBatchTimeout;
    delete recoveryTimeout;
    
    for (auto &kv : pendingPrepares) {
        delete kv.first;
    }
}

#if defined FAST_REPLY || defined FAST_BATCH || defined FAST_QUORUM_PRUNE
void VRReplica::ModifyKernelState() {
    struct paxos_ctr_state {
        enum ReplicaStatus state;
        int myIdx, leaderIdx, batchSize;
        __u64 view, lastOp;
    } state;

    state.state = status;
    state.view = view;
    state.lastOp = lastOp;
    state.myIdx = myIdx;
    state.leaderIdx = configuration.GetLeaderIndex(view);
    state.batchSize = batchSize;

    unsigned int zero = 0;
    if (bpf_map_update_elem(paxos_ctr_state_fd, &zero, &state, 0) < 0) {
        fprintf(stderr, "Error: bpf_map_update_elem \"ctr_state\" failed\n");
        exit(1);
    }
    // asd123www: maybe here is a polling?
}
#endif


uint64_t
VRReplica::GenerateNonce() const
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    return dis(gen);    
}

bool VRReplica::AmLeader() const {
    return (configuration.GetLeaderIndex(view) == myIdx);
}

void VRReplica::CommitUpTo(opnum_t upto) { // we can apply these requests in state machine!
    while (lastCommitted < upto) {
        Latency_Start(&executeAndReplyLatency);

        lastCommitted++;

        /* Find operation in log */
        const LogEntry *entry = log.Find(lastCommitted);
        if (!entry) {
            RPanic("Did not find operation " FMT_OPNUM " in log", lastCommitted);
        }

        /* asd123www:
            I think everyone should maintain all these state is because we all 
            may become leader in future.
            I should check the paper futher.
         */
        /* Execute it */
        RDebug("Executing request " FMT_OPNUM, lastCommitted);
        ReplyMessage reply;
        Execute(lastCommitted, entry->request, reply);

        reply.set_view(entry->viewstamp.view);
        reply.set_opnum(entry->viewstamp.opnum);
        reply.set_clientreqid(entry->request.clientreqid());
        
        /* Mark it as committed */
        log.SetStatus(lastCommitted, LOG_STATE_COMMITTED);

        // Store reply in the client table
        ClientTableEntry &cte =
            clientTable[entry->request.clientid()];
        if (cte.lastReqId <= entry->request.clientreqid()) {
            cte.lastReqId = entry->request.clientreqid();
            cte.replied = true;
            cte.reply = reply;            
        } else {
            // We've subsequently prepared another operation from the
            // same client. So this request must have been completed
            // at the client, and there's no need to record the
            // result.
        }

        /* Send reply */
        auto iter = clientAddresses.find(entry->request.clientid());
        if (iter != clientAddresses.end()) {
            transport->SendMessage(this, *iter->second, reply);
        }

        Latency_End(&executeAndReplyLatency);
    }
}

void
VRReplica::SendPrepareOKs(opnum_t oldLastOp)
{
    /* Send PREPAREOKs for new uncommitted operations */
    for (opnum_t i = oldLastOp; i <= lastOp; i++) {
        /* It has to be new *and* uncommitted */
        if (i <= lastCommitted) {
            continue;
        }

        const LogEntry *entry = log.Find(i);
        if (!entry) {
            RPanic("Did not find operation " FMT_OPNUM " in log", i);
        }
        ASSERT(entry->state == LOG_STATE_PREPARED);
        UpdateClientTable(entry->request);

        PrepareOKMessage reply;
        reply.set_view(view);
        reply.set_opnum(i);
        reply.set_replicaidx(myIdx);

        uint32_t my_data[3] = {(uint32_t)view, (uint32_t)i, (uint32_t)myIdx};
        RDebug("Sending PREPAREOK " FMT_VIEWSTAMP " for new uncommitted operation",
               reply.view(), reply.opnum());
    
        if (!(transport->SendMessageToReplica(this,
                                              configuration.GetLeaderIndex(view),
                                              reply, my_data))) {
            RWarning("Failed to send PrepareOK message to leader");
        }
    }
}

void
VRReplica::SendRecoveryMessages()
{
    RecoveryMessage m;
    m.set_replicaidx(myIdx);
    m.set_nonce(recoveryNonce);
    
    RNotice("Requesting recovery");
    if (!transport->SendMessageToAll(this, m)) {
        RWarning("Failed to send Recovery message to all replicas");
    }
}

// if we find our state is stale, then call this function.
void VRReplica::RequestStateTransfer() {
    RequestStateTransferMessage m;
    m.set_view(view);
    m.set_opnum(lastCommitted);

    if ((lastRequestStateTransferOpnum != 0) &&
        (lastRequestStateTransferView == view) &&
        (lastRequestStateTransferOpnum == lastCommitted)) {
        RDebug("Skipping state transfer request " FMT_VIEWSTAMP
               " because we already requested it", view, lastCommitted);
        return;
    }
    
    RNotice("Requesting state transfer: " FMT_VIEWSTAMP, view, lastCommitted);

    this->lastRequestStateTransferView = view;
    this->lastRequestStateTransferOpnum = lastCommitted;

    if (!transport->SendMessageToAll(this, m)) {
        RWarning("Failed to send RequestStateTransfer message to all replicas");
    }
}

void
VRReplica::EnterView(view_t newview)
{
    RNotice("Entering new view " FMT_VIEW, newview);

    view = newview;
    status = STATUS_NORMAL;
    lastBatchEnd = lastOp;
    batchComplete = true;

    recoveryTimeout->Stop();

    if (AmLeader()) {
        viewChangeTimeout->Stop();
        nullCommitTimeout->Start();
    } else {
        viewChangeTimeout->Start();
        nullCommitTimeout->Stop();
        resendPrepareTimeout->Stop();
        closeBatchTimeout->Stop();
    }

    prepareOKQuorum.Clear();
    startViewChangeQuorum.Clear();
    doViewChangeQuorum.Clear();
    recoveryResponseQuorum.Clear();
}

// if can't hear the heart beat from leader, then do the view change!
void VRReplica::StartViewChange(view_t newview) {
    RNotice("Starting view change for view " FMT_VIEW, newview);

    view = newview;
    status = STATUS_VIEW_CHANGE;

    viewChangeTimeout->Reset();
    nullCommitTimeout->Stop();
    resendPrepareTimeout->Stop();
    closeBatchTimeout->Stop();

    StartViewChangeMessage m;
    m.set_view(newview);
    m.set_replicaidx(myIdx);
    m.set_lastcommitted(lastCommitted);
    // broadcast this viewchange message.
    if (!transport->SendMessageToAll(this, m)) {
        RWarning("Failed to send StartViewChange message to all replicas");
    }
}

// heart beat message from leader.
void VRReplica::SendNullCommit() {
    CommitMessage cm;
    cm.set_view(this->view);
    cm.set_opnum(this->lastCommitted);

    ASSERT(AmLeader());

    if (!(transport->SendMessageToAll(this, cm
                                    #ifdef TC_BROADCAST
                                    , sgn_bits
                                    #endif
                                    ))) {
        RWarning("Failed to send null COMMIT message to all replicas");
    }
}

// asd123www: this can be maintained in XDP.
void VRReplica::UpdateClientTable(const Request &req) {
    ClientTableEntry &entry = clientTable[req.clientid()];

    ASSERT(entry.lastReqId <= req.clientreqid());

    if (entry.lastReqId == req.clientreqid()) {
        return;
    }

    entry.lastReqId = req.clientreqid();
    entry.replied = false;
    entry.reply.Clear();
}

void VRReplica::ResendPrepare() {
    ASSERT(AmLeader());
    if (lastOp == lastCommitted) {
        return;
    }
    RNotice("Resending prepare");
    if (!(transport->SendMessageToAll(this, lastPrepare
                                    #ifdef TC_BROADCAST
                                    , sgn_bits
                                    #endif
                                    ))) {
        RWarning("Failed to ressend prepare message to all replicas");
    }
}

void VRReplica::CloseBatch() {
    ASSERT(AmLeader());
    ASSERT(lastBatchEnd < lastOp); // the messages to be sent is not empty set.

    opnum_t batchStart = lastBatchEnd+1;
    
    RDebug("Sending batched prepare from " FMT_OPNUM
           " to " FMT_OPNUM,
           batchStart, lastOp);
    /* Send prepare messages */
    PrepareMessage p;
    p.set_view(view);
    p.set_opnum(lastOp);
    p.set_batchstart(batchStart);

    // batch the reqs in this interval, and send it all.
    for (opnum_t i = batchStart; i <= lastOp; i++) {
        Request *r = p.add_request();
        const LogEntry *entry = log.Find(i);
        ASSERT(entry != NULL);
        ASSERT(entry->viewstamp.view == view);
        ASSERT(entry->viewstamp.opnum == i);
        *r = entry->request;
    }
    lastPrepare = p;

    // asd123www: raw data, easy to parse in XDP.
#ifndef TC_BROADCAST
    uint32_t my_data[3] = {(uint32_t)view, (uint32_t)lastOp, (uint32_t)batchStart};
#else
    uint32_t my_data[3] = {(uint32_t)view | (1<<31), (uint32_t)lastOp, (uint32_t)batchStart};
#endif
    if (!(transport->SendMessageToAll(this, p, my_data))) {
        RWarning("Failed to send prepare message to all replicas");
    }
    lastBatchEnd = lastOp;
    batchComplete = false;

    resendPrepareTimeout->Reset();
    closeBatchTimeout->Stop();
}

void VRReplica::ReceiveMessage(const TransportAddress &remote, const string &type, const string &data) {

    #ifdef FAST_BATCH
    if (AmLeader()) {
        assert(ring_buffer__poll(rb_request, 0) >= 0);
    }
    #endif
    #ifdef FAST_REPLY
    if (!AmLeader()) {
        assert(ring_buffer__poll(rb_prepare, 0) >= 0);
    }
    #endif

    static RequestMessage request;
    static UnloggedRequestMessage unloggedRequest;
    static PrepareMessage prepare;
    static PrepareOKMessage prepareOK;
    static CommitMessage commit;
    static RequestStateTransferMessage requestStateTransfer;
    static StateTransferMessage stateTransfer;
    static StartViewChangeMessage startViewChange;
    static DoViewChangeMessage doViewChange;
    static StartViewMessage startView;
    static RecoveryMessage recovery;
    static RecoveryResponseMessage recoveryResponse;
    static std::string my_PrepareOK("specpaxos.vr.MyPrepareOK");
    
    if (type == request.GetTypeName()) { // HandleRequest, the leader's duty.
        request.ParseFromString(data);
        HandleRequest(remote, request);
    } else if (type == unloggedRequest.GetTypeName()) { // only by client, omit.
        unloggedRequest.ParseFromString(data);
        HandleUnloggedRequest(remote, unloggedRequest);
    } else if (type == prepare.GetTypeName()) { // HandlePrepare, in backup replica.
        prepare.ParseFromString(data);
        HandlePrepare(remote, prepare);
    } else if (type == prepareOK.GetTypeName()) { // HandlePrepareOK, the leader's duty.
        prepareOK.ParseFromString(data);
        HandlePrepareOK(remote, prepareOK);
    } else if (type == my_PrepareOK) { // our custom PrepareOK reply.
        const char *c = data.c_str();
        prepareOK.set_view(*(uint64_t *)c);
        prepareOK.set_opnum(*((uint64_t *)c + 1));
        prepareOK.set_replicaidx(*((uint32_t *)c + 4));
        HandlePrepareOK(remote, prepareOK);
    } else if (type == commit.GetTypeName()) { // HandleCommit, in back replica.
        commit.ParseFromString(data);
        HandleCommit(remote, commit);
    } else if (type == requestStateTransfer.GetTypeName()) { // checked.
        requestStateTransfer.ParseFromString(data);
        HandleRequestStateTransfer(remote, requestStateTransfer);
    } else if (type == stateTransfer.GetTypeName()) {  // checked.
        stateTransfer.ParseFromString(data);
        HandleStateTransfer(remote, stateTransfer);
    } else if (type == startViewChange.GetTypeName()) { // checked.
        startViewChange.ParseFromString(data);
        HandleStartViewChange(remote, startViewChange);
    } else if (type == doViewChange.GetTypeName()) { // checked.
        doViewChange.ParseFromString(data);
        HandleDoViewChange(remote, doViewChange);
    } else if (type == startView.GetTypeName()) { // checked.
        startView.ParseFromString(data);
        HandleStartView(remote, startView);
    } else if (type == recovery.GetTypeName()) { // recovery is too rare, omit here.
        recovery.ParseFromString(data);
        HandleRecovery(remote, recovery);
    } else if (type == recoveryResponse.GetTypeName()) { // recovery is too rare, omit here.
        recoveryResponse.ParseFromString(data);
        HandleRecoveryResponse(remote, recoveryResponse);
    } else {
        RPanic("Received unexpected message type in VR proto: %s",
              type.c_str());
    }

    // asd123www: add logic here if we have handle view-change.
}


void VRReplica::HandleRequest(const TransportAddress &remote,
                         const RequestMessage &msg) {
    viewstamp_t v;
    Latency_Start(&requestLatency);
    
    if (status != STATUS_NORMAL) { // don't handle request.
        RNotice("Ignoring request due to abnormal status");
        Latency_EndType(&requestLatency, 'i');
        return;
    }

    if (!AmLeader()) { // only leader should handle request.
        RDebug("Ignoring request because I'm not the leader");
        Latency_EndType(&requestLatency, 'i');
        return;
    }

    // Save the client's address
    clientAddresses.erase(msg.req().clientid());
    clientAddresses.insert(
        std::pair<uint64_t, std::unique_ptr<TransportAddress> >(
            msg.req().clientid(),
            std::unique_ptr<TransportAddress>(remote.clone())));

    // Check the client table to see if this is a duplicate request
    auto kv = clientTable.find(msg.req().clientid());
    if (kv != clientTable.end()) {
        const ClientTableEntry &entry = kv->second;
        if (msg.req().clientreqid() < entry.lastReqId) {
            RNotice("Ignoring stale request");
            Latency_EndType(&requestLatency, 's');
            return;
        }
        if (msg.req().clientreqid() == entry.lastReqId) {
            // This is a duplicate request. Resend the reply if we
            // have one. We might not have a reply to resend if we're
            // waiting for the other replicas; in that case, just
            // discard the request.
            if (entry.replied) {
                RNotice("Received duplicate request; resending reply");
                if (!(transport->SendMessage(this, remote,
                                             entry.reply))) {
                    RWarning("Failed to resend reply to client");
                }
                Latency_EndType(&requestLatency, 'r');
                return;
            } else {
                RNotice("Received duplicate request but no reply available; ignoring");
                Latency_EndType(&requestLatency, 'd');
                return;
            }
        }
    }

    // Update the client table
    UpdateClientTable(msg.req());

    // Leader Upcall
    bool replicate = false;
    string res;
    LeaderUpcall(lastCommitted, msg.req().op(), replicate, res); // set replicate and copy op() to res.
    ClientTableEntry &cte = clientTable[msg.req().clientid()];

    // Check whether this request should be committed to replicas
    if (!replicate) {
        RDebug("Executing request failed. Not committing to replicas");
        ReplyMessage reply;

        reply.set_reply(res);
        reply.set_view(0);
        reply.set_opnum(0);
        reply.set_clientreqid(msg.req().clientreqid());
        cte.replied = true;
        cte.reply = reply;
        transport->SendMessage(this, remote, reply);
        Latency_EndType(&requestLatency, 'f');
    } else { // here is the normal execution path.
        Request request;
        request.set_op(res);
        request.set_clientid(msg.req().clientid());
        request.set_clientreqid(msg.req().clientreqid());
    
        /* Assign it an opnum */ // increasing by one.
        ++this->lastOp;
        v.view = this->view;
        v.opnum = this->lastOp;

        RDebug("Received REQUEST, assigning " FMT_VIEWSTAMP, VA_VIEWSTAMP(v));

        /* Add the request to my log */
        log.Append(v, request, LOG_STATE_PREPARED); // state of this entry in log is PREPARED.

        if (batchComplete ||
            (lastOp - lastBatchEnd+1 > (unsigned int)batchSize)) {
            CloseBatch();
        } else {
            RDebug("Keeping in batch");
            if (!closeBatchTimeout->Active()) {
                closeBatchTimeout->Start();
            }
        }

        nullCommitTimeout->Reset(); // asd123www: 这合理吗？我这次又不一定发信息....
        Latency_End(&requestLatency);
    }
}

// asd123www: ?????only client send this message, rare case.
void VRReplica::HandleUnloggedRequest(const TransportAddress &remote, const UnloggedRequestMessage &msg) {
    if (status != STATUS_NORMAL) {
        // Not clear if we should ignore this or just let the request
        // go ahead, but this seems reasonable.
        RNotice("Ignoring unlogged request due to abnormal status");
        return;
    }

    UnloggedReplyMessage reply;
    
    Debug("Received unlogged request %s", (char *)msg.req().op().c_str());

    ExecuteUnlogged(msg.req(), reply);
    
    if (!(transport->SendMessage(this, remote, reply)))
        Warning("Failed to send reply message");
}


void VRReplica::HandlePrepare_Kernel(const proto::PrepareMessage &msg) {
    RDebug("Received PREPARE <" FMT_VIEW "," FMT_OPNUM "-" FMT_OPNUM ">",
           msg.view(), msg.batchstart(), msg.opnum());
    ASSERT(this->status == STATUS_NORMAL);
    ASSERT(msg.view() == this->view);
    ASSERT(msg.batchstart() <= msg.opnum());
    ASSERT_EQ(msg.opnum()-msg.batchstart()+1, (unsigned)msg.request_size());

    /* Add operations to the log */
    opnum_t op = msg.batchstart()-1;
    for (auto &req : msg.request()) {
        op++;
        if (op <= lastOp) continue;
        this->lastOp++;
        log.Append(viewstamp_t(msg.view(), op), req, LOG_STATE_PREPARED);
        UpdateClientTable(req);
    }
    ASSERT(op == msg.opnum());
}


void VRReplica::HandlePrepare(const TransportAddress &remote, const PrepareMessage &msg) {
    RDebug("Received PREPARE <" FMT_VIEW "," FMT_OPNUM "-" FMT_OPNUM ">",
           msg.view(), msg.batchstart(), msg.opnum());

    if (this->status != STATUS_NORMAL) { // no interaction.
        RDebug("Ignoring PREPARE due to abnormal status");
        return;
    }
    
    if (msg.view() < this->view) { // hear a stale  message, we shouldn't respond to that.
        RDebug("Ignoring PREPARE due to stale view");
        return;
    }

    if (msg.view() > this->view) { // trigger view change! we laid behind.
        RequestStateTransfer();
        pendingPrepares.push_back(std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
        return;
    }

    if (AmLeader()) { // leader shouldn't receive this message.
        RPanic("Unexpected PREPARE: I'm the leader of this view");
    }

    // sanity checks.
    ASSERT(msg.batchstart() <= msg.opnum());
    ASSERT_EQ(msg.opnum()-msg.batchstart()+1, (unsigned)msg.request_size());

    viewChangeTimeout->Reset(); // asd123www: how to maintain this in XDP? maybe pulling from kernel if this is triggered.

    if (msg.opnum() <= this->lastOp) { // we have received...
        RDebug("Ignoring PREPARE; already prepared that operation");
        // Resend the prepareOK message
        PrepareOKMessage reply;
        reply.set_view(msg.view());
        reply.set_opnum(msg.opnum());
        reply.set_replicaidx(myIdx);


        uint32_t my_data[3] = {(uint32_t)msg.view(), (uint32_t)msg.opnum(), (uint32_t)myIdx};
        if (!(transport->SendMessageToReplica(this,
                                              configuration.GetLeaderIndex(view),
                                              reply, my_data))) {
            RWarning("Failed to send PrepareOK message to leader");
        }
        return;
    }

    /* asd123www: From <Viewstamped Replication Revisited>:
        a backup won’t accept a prepare with op-number n
        until it has entries for all earlier requests in its log. */
    if (msg.batchstart() > this->lastOp+1) { 
        RequestStateTransfer();
        pendingPrepares.push_back(std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
        return;
    }

    /* Add operations to the log */
    opnum_t op = msg.batchstart()-1;
    for (auto &req : msg.request()) {
        op++;
        if (op <= lastOp) continue;
        this->lastOp++;
        log.Append(viewstamp_t(msg.view(), op), req, LOG_STATE_PREPARED);
        UpdateClientTable(req);
    }
    ASSERT(op == msg.opnum());
    
    /* Build reply and send it to the leader */
    PrepareOKMessage reply;
    reply.set_view(msg.view());
    reply.set_opnum(msg.opnum());
    reply.set_replicaidx(myIdx);

    uint32_t my_data[3] = {(uint32_t)msg.view(), (uint32_t)msg.opnum(), (uint32_t)myIdx};
    if (!(transport->SendMessageToReplica(this,
                                          configuration.GetLeaderIndex(view),
                                          reply, my_data))) {
        RWarning("Failed to send PrepareOK message to leader");
    }
}


void VRReplica::HandlePrepareOK(const TransportAddress &remote, const PrepareOKMessage &msg) {
    RDebug("Received PREPAREOK <" FMT_VIEW ", "
           FMT_OPNUM  "> from replica %d",
           msg.view(), msg.opnum(), msg.replicaidx());

    if (this->status != STATUS_NORMAL) {
        RDebug("Ignoring PREPAREOK due to abnormal status");
        return;
    }

    if (msg.view() < this->view) {
        RDebug("Ignoring PREPAREOK due to stale view");
        return;
    }

    if (msg.view() > this->view) {
        RequestStateTransfer();
        return;
    }

    if (!AmLeader()) {
        RWarning("Ignoring PREPAREOK because I'm not the leader");
        return;
    }
    
    viewstamp_t vs = { msg.view(), msg.opnum() };
#ifndef FAST_QUORUM_PRUNE
    if (auto msgs = (prepareOKQuorum.AddAndCheckForQuorum(vs, msg.replicaidx(), msg))) {
#endif
        /*
         * We have a quorum of PrepareOK messages for this
         * opnumber. Execute it and all previous operations.
         *
         * (Note that we might have already executed it. That's fine,
         * we just won't do anything.)
         *
         * This also notifies the client of the result.
         */
        CommitUpTo(msg.opnum()); // leader then knows these messages have been committed, then we can apply to the state-machine.

#ifndef FAST_QUORUM_PRUNE
        if (msgs->size() >= (unsigned)configuration.QuorumSize()) { // not the critical point.. e.g. Quorum() - 1
            return;
        }
#endif
        /*
         * Send COMMIT message to the other replicas.
         *
         * This can be done asynchronously, so it really ought to be
         * piggybacked on the next PREPARE or something.
         */
        CommitMessage cm;
        cm.set_view(this->view);
        cm.set_opnum(this->lastCommitted);

        if (!(transport->SendMessageToAll(this, cm
                                        #ifdef TC_BROADCAST
                                        , sgn_bits
                                        #endif
                                        ))) {
            RWarning("Failed to send COMMIT message to all replicas");
        }

        nullCommitTimeout->Reset(); // we will broadcast a message, then this can be cancelled.

        // asd123www: what is adaptive batching????
        // XXX Adaptive batching -- make this configurable
        if (lastBatchEnd == msg.opnum()) {
            batchComplete = true;
            if  (lastOp > lastBatchEnd) {
                CloseBatch();
            }
        }
#ifndef FAST_QUORUM_PRUNE
    }
#endif
}


void VRReplica::HandleCommit(const TransportAddress &remote, const CommitMessage &msg) {
    RDebug("Received COMMIT " FMT_VIEWSTAMP, msg.view(), msg.opnum());

    if (this->status != STATUS_NORMAL) {
        RDebug("Ignoring COMMIT due to abnormal status");
        return;
    }
    
    if (msg.view() < this->view) {
        RDebug("Ignoring COMMIT due to stale view");
        return;
    }

    if (msg.view() > this->view) {
        RequestStateTransfer();
        return;
    }

    if (AmLeader()) {
        RPanic("Unexpected COMMIT: I'm the leader of this view");
    }

    viewChangeTimeout->Reset();

    if (msg.opnum() <= this->lastCommitted) {
        RDebug("Ignoring COMMIT; already committed that operation");
        return;
    }

    if (msg.opnum() > this->lastOp) { // we don't have this request...
        RequestStateTransfer();
        return;
    }

    CommitUpTo(msg.opnum());
}


void VRReplica::HandleRequestStateTransfer(const TransportAddress &remote, const RequestStateTransferMessage &msg) {
    RDebug("Received REQUESTSTATETRANSFER " FMT_VIEWSTAMP,
           msg.view(), msg.opnum());

    if (status != STATUS_NORMAL) {
        RDebug("Ignoring REQUESTSTATETRANSFER due to abnormal status");
        return;
    }

    if (msg.view() > view) {
        RequestStateTransfer();
        return;
    }

    RNotice("Sending state transfer from " FMT_VIEWSTAMP " to "
            FMT_VIEWSTAMP,
            msg.view(), msg.opnum(), view, lastCommitted);

    StateTransferMessage reply;
    reply.set_view(view);
    reply.set_opnum(lastCommitted);
    
    log.Dump(msg.opnum()+1, reply.mutable_entries());
    // send back our state in log.
    transport->SendMessage(this, remote, reply);
}

// update our state.
void VRReplica::HandleStateTransfer(const TransportAddress &remote, const StateTransferMessage &msg) {
    RDebug("Received STATETRANSFER " FMT_VIEWSTAMP, msg.view(), msg.opnum());
    
    if (msg.view() < view) {
        RWarning("Ignoring state transfer for older view");
        return;
    }
    
    opnum_t oldLastOp = lastOp;
    
    /* Install the new log entries */
    for (auto newEntry : msg.entries()) {
        if (newEntry.opnum() <= lastCommitted) {
            // Already committed this operation; nothing to be done.
#if PARANOID
            const LogEntry *entry = log.Find(newEntry.opnum());
            ASSERT(entry->viewstamp.opnum == newEntry.opnum());
            ASSERT(entry->viewstamp.view == newEntry.view());
//          ASSERT(entry->request == newEntry.request());
#endif
        } else if (newEntry.opnum() <= lastOp) {
            // We already have an entry with this opnum, but maybe
            // it's from an older view?
            const LogEntry *entry = log.Find(newEntry.opnum());
            ASSERT(entry->viewstamp.opnum == newEntry.opnum());
            ASSERT(entry->viewstamp.view <= newEntry.view());
            
            if (entry->viewstamp.view == newEntry.view()) {
                // We already have this operation in our log.
                ASSERT(entry->state == LOG_STATE_PREPARED);
#if PARANOID
//              ASSERT(entry->request == newEntry.request());                
#endif
            } else {
                // Our operation was from an older view, so obviously
                // it didn't survive a view change. Throw out any
                // later log entries and replace with this one.
                ASSERT(entry->state != LOG_STATE_COMMITTED);
                log.RemoveAfter(newEntry.opnum());
                lastOp = newEntry.opnum();
                oldLastOp = lastOp;

                viewstamp_t vs = { newEntry.view(), newEntry.opnum() };
                log.Append(vs, newEntry.request(), LOG_STATE_PREPARED);
            }
        } else {
            // This is a new operation to us. Add it to the log.
            ASSERT(newEntry.opnum() == lastOp+1);
            
            lastOp++;
            viewstamp_t vs = { newEntry.view(), newEntry.opnum() };
            log.Append(vs, newEntry.request(), LOG_STATE_PREPARED);
        }
    }
    

    if (msg.view() > view) {
        EnterView(msg.view());
    }

    /* Execute committed operations */
    ASSERT(msg.opnum() <= lastOp);
    CommitUpTo(msg.opnum());
    SendPrepareOKs(oldLastOp);

    // Process pending prepares
    std::list<std::pair<TransportAddress *, PrepareMessage> >pending = pendingPrepares;
    pendingPrepares.clear();
    for (auto & msgpair : pendingPrepares) {
        RDebug("Processing pending prepare message");
        HandlePrepare(*msgpair.first, msgpair.second);
        delete msgpair.first;
    }
}

void VRReplica::HandleStartViewChange(const TransportAddress &remote, const StartViewChangeMessage &msg) {
    RDebug("Received STARTVIEWCHANGE " FMT_VIEW " from replica %d",
           msg.view(), msg.replicaidx());

    if (msg.view() < view) {
        RDebug("Ignoring STARTVIEWCHANGE for older view");
        return;
    }

    if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE)) {
        RDebug("Ignoring STARTVIEWCHANGE for current view");
        return;
    }

    if ((status != STATUS_VIEW_CHANGE) || (msg.view() > view)) {
        RWarning("Received StartViewChange for view " FMT_VIEW
                 "from replica %d", msg.view(), msg.replicaidx());
        StartViewChange(msg.view());
    }

    ASSERT(msg.view() == view);
    
    // if reach quorum, then broadcast do view change message.
    if (auto msgs =
        startViewChangeQuorum.AddAndCheckForQuorum(msg.view(),
                                                   msg.replicaidx(),
                                                   msg)) {
        int leader = configuration.GetLeaderIndex(view);
        // Don't try to send a DoViewChange message to ourselves
        if (leader != myIdx) {            
            DoViewChangeMessage dvc;
            dvc.set_view(view);
            dvc.set_lastnormalview(log.LastViewstamp().view);
            dvc.set_lastop(lastOp);
            dvc.set_lastcommitted(lastCommitted);
            dvc.set_replicaidx(myIdx);

            // Figure out how much of the log to include
            opnum_t minCommitted = std::min_element(
                msgs->begin(), msgs->end(),
                [](decltype(*msgs->begin()) a,
                   decltype(*msgs->begin()) b) {
                    return a.second.lastcommitted() < b.second.lastcommitted();
                })->second.lastcommitted();
            minCommitted = std::min(minCommitted, lastCommitted);
            
            log.Dump(minCommitted,
                     dvc.mutable_entries());

            if (!(transport->SendMessageToReplica(this, leader, dvc))) {
                RWarning("Failed to send DoViewChange message to leader of new view");
            }
        }
    }
}


void VRReplica::HandleDoViewChange(const TransportAddress &remote, const DoViewChangeMessage &msg) {
    RDebug("Received DOVIEWCHANGE " FMT_VIEW " from replica %d, "
           "lastnormalview=" FMT_VIEW " op=" FMT_OPNUM " committed=" FMT_OPNUM,
           msg.view(), msg.replicaidx(),
           msg.lastnormalview(), msg.lastop(), msg.lastcommitted());

    if (msg.view() < view) {
        RDebug("Ignoring DOVIEWCHANGE for older view");
        return;
    }

    if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE)) {
        RDebug("Ignoring DOVIEWCHANGE for current view");
        return;
    }

    if ((status != STATUS_VIEW_CHANGE) || (msg.view() > view)) {
        // It's superfluous to send the StartViewChange messages here,
        // but harmless...
        RWarning("Received DoViewChange for view " FMT_VIEW
                 "from replica %d", msg.view(), msg.replicaidx());
        StartViewChange(msg.view());
    }

    ASSERT(configuration.GetLeaderIndex(msg.view()) == myIdx);
    
    auto msgs = doViewChangeQuorum.AddAndCheckForQuorum(msg.view(),
                                                        msg.replicaidx(),
                                                        msg);
    if (msgs != NULL) {
        // Find the response with the most up to date log, i.e. the
        // one with the latest viewstamp
        view_t latestView = log.LastViewstamp().view;
        opnum_t latestOp = log.LastViewstamp().opnum;
        DoViewChangeMessage *latestMsg = NULL;

        for (auto kv : *msgs) {
            DoViewChangeMessage &x = kv.second;
            if ((x.lastnormalview() > latestView) ||
                (((x.lastnormalview() == latestView) &&
                  (x.lastop() > latestOp)))) {
                latestView = x.lastnormalview();
                latestOp = x.lastop();
                latestMsg = &x;
            }
        }

        // Install the new log. We might not need to do this, if our
        // log was the most current one.
        if (latestMsg != NULL) {
            RDebug("Selected log from replica %d with lastop=" FMT_OPNUM,
                   latestMsg->replicaidx(), latestMsg->lastop());
            if (latestMsg->entries_size() == 0) {
                // There weren't actually any entries in the
                // log. That should only happen in the corner case
                // that everyone already had the entire log, maybe
                // because it actually is empty.
                ASSERT(lastCommitted == msg.lastcommitted());
                ASSERT(msg.lastop() == msg.lastcommitted());
            } else {
                if (latestMsg->entries(0).opnum() > lastCommitted+1) {
                    RPanic("Received log that didn't include enough entries to install it");
                }
                
                log.RemoveAfter(latestMsg->lastop()+1);
                log.Install(latestMsg->entries().begin(),
                            latestMsg->entries().end());
            }
        } else {
            RDebug("My log is most current, lastnormalview=" FMT_VIEW " lastop=" FMT_OPNUM,
                   log.LastViewstamp().view, lastOp);
        }

        // How much of the log should we include when we send the
        // STARTVIEW message? Start from the lowest committed opnum of
        // any of the STARTVIEWCHANGE or DOVIEWCHANGE messages we got.
        //
        // We need to compute this before we enter the new view
        // because the saved messages will go away.
        auto svcs = startViewChangeQuorum.GetMessages(view);
        opnum_t minCommittedSVC = std::min_element(
            svcs.begin(), svcs.end(),
            [](decltype(*svcs.begin()) a,
               decltype(*svcs.begin()) b) {
                return a.second.lastcommitted() < b.second.lastcommitted();
            })->second.lastcommitted();
        opnum_t minCommittedDVC = std::min_element(
            msgs->begin(), msgs->end(),
            [](decltype(*msgs->begin()) a,
               decltype(*msgs->begin()) b) {
                return a.second.lastcommitted() < b.second.lastcommitted();
            })->second.lastcommitted();
        opnum_t minCommitted = std::min(minCommittedSVC, minCommittedDVC);
        minCommitted = std::min(minCommitted, lastCommitted);

        EnterView(msg.view());

        ASSERT(AmLeader());
        
        lastOp = latestOp;
        if (latestMsg != NULL) {
            CommitUpTo(latestMsg->lastcommitted());
        }

        // Send a STARTVIEW message with the new log
        StartViewMessage sv;
        sv.set_view(view);
        sv.set_lastop(lastOp);
        sv.set_lastcommitted(lastCommitted);
        
        log.Dump(minCommitted, sv.mutable_entries());

        if (!(transport->SendMessageToAll(this, sv))) {
            RWarning("Failed to send StartView message to all replicas");
        }
    }    
}

void
VRReplica::HandleStartView(const TransportAddress &remote,
                           const StartViewMessage &msg)
{
    RDebug("Received STARTVIEW " FMT_VIEW 
          " op=" FMT_OPNUM " committed=" FMT_OPNUM " entries=%d",
          msg.view(), msg.lastop(), msg.lastcommitted(), msg.entries_size());
    RDebug("Currently in view " FMT_VIEW " op " FMT_OPNUM " committed " FMT_OPNUM,
          view, lastOp, lastCommitted);

    if (msg.view() < view) {
        RWarning("Ignoring STARTVIEW for older view");
        return;
    }

    if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE)) {
        RWarning("Ignoring STARTVIEW for current view");
        return;
    }

    ASSERT(configuration.GetLeaderIndex(msg.view()) != myIdx);

    if (msg.entries_size() == 0) {
        ASSERT(msg.lastcommitted() == lastCommitted);
        ASSERT(msg.lastop() == msg.lastcommitted());
    } else {
        if (msg.entries(0).opnum() > lastCommitted+1) {
            RPanic("Not enough entries in STARTVIEW message to install new log");
        }
        
        // Install the new log
        log.RemoveAfter(msg.lastop()+1);
        log.Install(msg.entries().begin(),
                    msg.entries().end());        
    }


    EnterView(msg.view());
    opnum_t oldLastOp = lastOp;
    lastOp = msg.lastop();

    ASSERT(!AmLeader());

    CommitUpTo(msg.lastcommitted());
    SendPrepareOKs(oldLastOp);
}

void
VRReplica::HandleRecovery(const TransportAddress &remote,
                          const RecoveryMessage &msg)
{
    RDebug("Received RECOVERY from replica %d", msg.replicaidx());

    if (status != STATUS_NORMAL) {
        RDebug("Ignoring RECOVERY due to abnormal status");
        return;
    }

    RecoveryResponseMessage reply;
    reply.set_replicaidx(myIdx);
    reply.set_view(view);
    reply.set_nonce(msg.nonce());
    if (AmLeader()) {
        reply.set_lastcommitted(lastCommitted);
        reply.set_lastop(lastOp);
        log.Dump(0, reply.mutable_entries());
    }

    if (!(transport->SendMessage(this, remote, reply))) {
        RWarning("Failed to send recovery response");
    }
    return;
}

void
VRReplica::HandleRecoveryResponse(const TransportAddress &remote,
                                  const RecoveryResponseMessage &msg)
{
    RDebug("Received RECOVERYRESPONSE from replica %d",
           msg.replicaidx());

    if (status != STATUS_RECOVERING) {
        RDebug("Ignoring RECOVERYRESPONSE because we're not recovering");
        return;
    }

    if (msg.nonce() != recoveryNonce) {
        RNotice("Ignoring recovery response because nonce didn't match");
        return;
    }

    auto msgs = recoveryResponseQuorum.AddAndCheckForQuorum(msg.nonce(),
                                                            msg.replicaidx(),
                                                            msg);
    if (msgs != NULL) {
        view_t highestView = 0;
        for (const auto &kv : *msgs) {
            if (kv.second.view() > highestView) {
                highestView = kv.second.view();
            }
        }
        
        int leader = configuration.GetLeaderIndex(highestView);
        ASSERT(leader != myIdx);
        auto leaderResponse = msgs->find(leader);
        if ((leaderResponse == msgs->end()) ||
            (leaderResponse->second.view() != highestView)) {
            RDebug("Have quorum of RECOVERYRESPONSE messages, "
                   "but still need to wait for one from the leader");
            return;
        }

        Notice("Recovery completed");
        
        log.Install(leaderResponse->second.entries().begin(),
                    leaderResponse->second.entries().end());        
        EnterView(leaderResponse->second.view());
        lastOp = leaderResponse->second.lastop();
        CommitUpTo(leaderResponse->second.lastcommitted());
    }
}

} // namespace specpaxos::vr
} // namespace specpaxos
