/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file has the logic which the middleman process uses to forward IPDL
// messages from the recording process to the UI process, and from the UI
// process to either itself, the recording process, or both.

#include "ParentInternal.h"

#include "mozilla/dom/PBrowserChild.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/PWindowGlobalChild.h"
#include "mozilla/layers/CompositorBridgeChild.h"

namespace mozilla {
namespace recordreplay {
namespace parent {

static bool HandleMessageInMiddleman(ipc::Side aSide,
                                     const IPC::Message& aMessage) {
  IPC::Message::msgid_t type = aMessage.type();

  if (aSide == ipc::ParentSide) {
    return false;
  }

  // Handle messages that should be sent to both the middleman and the
  // child process.
  if (  // Initialization that must be performed in both processes.
      type == dom::PContent::Msg_ConstructBrowser__ID ||
      type == dom::PContent::Msg_RegisterBrowsingContextGroup__ID ||
      type == dom::PContent::Msg_RegisterChrome__ID ||
      type == dom::PContent::Msg_SetXPCOMProcessAttributes__ID ||
      type == dom::PContent::Msg_UpdateSharedData__ID ||
      type == dom::PContent::Msg_SetProcessSandbox__ID ||
      // Graphics messages that affect both processes.
      type == dom::PBrowser::Msg_InitRendering__ID ||
      type == dom::PBrowser::Msg_SetDocShellIsActive__ID ||
      type == dom::PBrowser::Msg_RenderLayers__ID ||
      type == dom::PBrowser::Msg_UpdateDimensions__ID ||
      // These messages perform some graphics related initialization.
      type == dom::PBrowser::Msg_LoadURL__ID ||
      type == dom::PBrowser::Msg_Show__ID ||
      // May be loading devtools code that runs in the middleman process.
      type == dom::PBrowser::Msg_LoadRemoteScript__ID ||
      // May be sending a message for receipt by devtools code.
      type == dom::PBrowser::Msg_AsyncMessage__ID ||
      // Teardown that must be performed in both processes.
      type == dom::PBrowser::Msg_Destroy__ID) {
    dom::ContentChild* contentChild = dom::ContentChild::GetSingleton();

    if (type >= dom::PBrowser::PBrowserStart &&
        type <= dom::PBrowser::PBrowserEnd) {
      // Ignore messages sent from the parent to browsers that do not have an
      // actor in the middleman process. PBrowser may be allocated on either
      // side of the IPDL channel, and when allocated by the recording child
      // there will not be a corresponding actor in the middleman.
      nsTArray<dom::PBrowserChild*> browsers;
      contentChild->ManagedPBrowserChild(browsers);
      bool found = false;
      for (ipc::IProtocol* child : browsers) {
        if (child->Id() == aMessage.routing_id()) {
          found = true;
          break;
        }
      }
      if (!found) {
        return false;
      }
    }

    ipc::IProtocol::Result r =
        contentChild->PContentChild::OnMessageReceived(aMessage);
    MOZ_RELEASE_ASSERT(r == ipc::IProtocol::MsgProcessed);
    return false;
  }

  // Handle messages that should only be sent to the middleman.
  if (  // Initialization that should only happen in the middleman.
      type == dom::PContent::Msg_InitRendering__ID ||
      // Teardown that should only happen in the middleman.
      type == dom::PContent::Msg_Shutdown__ID) {
    ipc::IProtocol::Result r =
        dom::ContentChild::GetSingleton()->PContentChild::OnMessageReceived(
            aMessage);
    MOZ_RELEASE_ASSERT(r == ipc::IProtocol::MsgProcessed);
    return true;
  }

  // The content process has its own compositor, so compositor messages from
  // the UI process should only be handled in the middleman.
  if (type >= layers::PCompositorBridge::PCompositorBridgeStart &&
      type <= layers::PCompositorBridge::PCompositorBridgeEnd) {
    layers::CompositorBridgeChild* compositorChild =
        layers::CompositorBridgeChild::Get();
    ipc::IProtocol::Result r = compositorChild->OnMessageReceived(aMessage);
    MOZ_RELEASE_ASSERT(r == ipc::IProtocol::MsgProcessed);
    return true;
  }

  // PWindowGlobal messages could be going to actors in either process.
  // Receive them here if there is an actor with the right routing ID.
  if (type >= dom::PWindowGlobal::PWindowGlobalStart &&
      type <= dom::PWindowGlobal::PWindowGlobalEnd) {
    dom::ContentChild* contentChild = dom::ContentChild::GetSingleton();

    ipc::IProtocol* actor = contentChild->Lookup(aMessage.routing_id());
    if (actor) {
      ipc::IProtocol::Result r =
          contentChild->PContentChild::OnMessageReceived(aMessage);
      MOZ_RELEASE_ASSERT(r == ipc::IProtocol::MsgProcessed);
      return true;
    }
    return false;
  }

  // Asynchronous replies to messages originally sent by the middleman need to
  // be handled in the middleman.
  if (ipc::MessageChannel::MessageOriginatesFromMiddleman(aMessage)) {
    return true;
  }

  return false;
}

static void BeginShutdown() {
  // If there is a channel error or anything that could result from the child
  // crashing, cleanly shutdown this process so that we don't generate a
  // separate minidump which masks the initial failure.
  MainThreadMessageLoop()->PostTask(NewRunnableFunction("Shutdown", Shutdown));
}

// Runnables forwarding messages which need to execute on the main thread.
// Protected by gMonitor.
static StaticInfallibleVector<RefPtr<nsIRunnable>> gMainThreadRunnables;

// Whether a task has been posted to the main thread to process any runnables.
// Protected by gMonitor.
static bool gPostedProcessMainThreadRunnables;

static void ProcessMainThreadRunnables() {
  MonitorAutoLock lock(*gMonitor);
  for (size_t i = 0; i < gMainThreadRunnables.length(); i++) {
    RefPtr<nsIRunnable> runnable = gMainThreadRunnables[i];
    MonitorAutoUnlock unlock(*gMonitor);
    runnable->Run();
  }
  gMainThreadRunnables.clear();
  gPostedProcessMainThreadRunnables = false;
}

void DispatchToMainThread(already_AddRefed<nsIRunnable>&& aRunnable) {
  MonitorAutoLock lock(*gMonitor);
  gMainThreadRunnables.append(aRunnable);
  if (!gPostedProcessMainThreadRunnables) {
    MainThreadMessageLoop()->PostTask(NewRunnableFunction(
          "ProcessMainThreadRunnables", ProcessMainThreadRunnables));
      gPostedProcessMainThreadRunnables = true;
  }
}

class MiddlemanProtocol : public ipc::IToplevelProtocol {
 public:
  // The side which the forwarded messages are being sent to.
  ipc::Side mSide;

  MiddlemanProtocol* mOpposite;
  MessageLoop* mOppositeMessageLoop;

  explicit MiddlemanProtocol(ipc::Side aSide)
      : ipc::IToplevelProtocol("MiddlemanProtocol", PContentMsgStart, aSide),
        mSide(aSide),
        mOpposite(nullptr),
        mOppositeMessageLoop(nullptr) {}

  virtual void RemoveManagee(int32_t, IProtocol*) override {
    MOZ_CRASH("MiddlemanProtocol::RemoveManagee");
  }

  virtual void DeallocManagee(int32_t, IProtocol*) override {
    MOZ_CRASH("MiddlemanProtocol::DeallocManagee");
  }

  virtual void AllManagedActors(
      nsTArray<RefPtr<ipc::ActorLifecycleProxy>>& aActors) const override {
    aActors.Clear();
  }

  // Post a runnable for the other message loop's thread.
  void PostOppositeRunnable(already_AddRefed<nsIRunnable>&& aRunnable) {
    if (mSide == ipc::ChildSide) {
      mOppositeMessageLoop->PostTask(std::move(aRunnable));
    } else {
      DispatchToMainThread(std::move(aRunnable));
    }
  }

  static void ForwardMessageAsync(MiddlemanProtocol* aProtocol,
                                  Message* aMessage) {
    PrintSpew("ForwardAsyncMsgFrom %s %s %d\n",
              (aProtocol->mSide == ipc::ChildSide) ? "Child" : "Parent",
              IPC::StringFromIPCMessageType(aMessage->type()),
              (int)aMessage->routing_id());
    if (!aProtocol->GetIPCChannel()->Send(aMessage)) {
      MOZ_RELEASE_ASSERT(aProtocol->mSide == ipc::ParentSide);
      BeginShutdown();
    }
  }

  virtual Result OnMessageReceived(const Message& aMessage) override {
    // If we do not have a recording process then just see if the message can
    // be handled in the middleman.
    if (!mOppositeMessageLoop) {
      MOZ_RELEASE_ASSERT(mSide == ipc::ChildSide);
      HandleMessageInMiddleman(mSide, aMessage);
      return MsgProcessed;
    }

    // Copy the message first, since HandleMessageInMiddleman may destructively
    // modify it through OnMessageReceived calls.
    Message* nMessage = new Message();
    nMessage->CopyFrom(aMessage);

    if (HandleMessageInMiddleman(mSide, aMessage)) {
      delete nMessage;
      return MsgProcessed;
    }

    PostOppositeRunnable(NewRunnableFunction(
        "ForwardMessageAsync", ForwardMessageAsync, mOpposite, nMessage));
    return MsgProcessed;
  }

  Message* mSyncMessage = nullptr;
  Message* mSyncMessageReply = nullptr;
  bool mSyncMessageIsCall = false;

  void MaybeSendSyncMessage() {
    MonitorAutoLock lock(*gMonitor);

    if (!mSyncMessage) {
      return;
    }

    PrintSpew("ForwardSyncMsg %s\n",
              IPC::StringFromIPCMessageType(mSyncMessage->type()));

    MOZ_RELEASE_ASSERT(!mSyncMessageReply);
    mSyncMessageReply = new Message();
    if (mSyncMessageIsCall
            ? !mOpposite->GetIPCChannel()->Call(mSyncMessage, mSyncMessageReply)
            : !mOpposite->GetIPCChannel()->Send(mSyncMessage,
                                                mSyncMessageReply)) {
      MOZ_RELEASE_ASSERT(mSide == ipc::ChildSide);
      BeginShutdown();
    }

    mSyncMessage = nullptr;

    gMonitor->NotifyAll();
  }

  static void StaticMaybeSendSyncMessage(MiddlemanProtocol* aProtocol) {
    aProtocol->MaybeSendSyncMessage();
  }

  void HandleSyncMessage(const Message& aMessage, Message*& aReply,
                         bool aCall) {
    MOZ_RELEASE_ASSERT(mOppositeMessageLoop);

    mSyncMessage = new Message();
    mSyncMessage->CopyFrom(aMessage);
    mSyncMessageIsCall = aCall;

    PostOppositeRunnable(NewRunnableFunction(
        "StaticMaybeSendSyncMessage", StaticMaybeSendSyncMessage, this));

    if (mSide == ipc::ChildSide) {
      MOZ_CRASH("NYI");
    } else {
      MonitorAutoLock lock(*gMonitor);

      // If the main thread is blocked waiting for the recording child to pause,
      // wake it up so it can call MaybeHandleForwardedMessages().
      gMonitor->NotifyAll();

      while (!mSyncMessageReply) {
        gMonitor->Wait();
      }
    }

    aReply = mSyncMessageReply;
    mSyncMessageReply = nullptr;

    PrintSpew("SyncMsgDone\n");
  }

  virtual Result OnMessageReceived(const Message& aMessage,
                                   Message*& aReply) override {
    HandleSyncMessage(aMessage, aReply, false);
    return MsgProcessed;
  }

  virtual Result OnCallReceived(const Message& aMessage,
                                Message*& aReply) override {
    HandleSyncMessage(aMessage, aReply, true);
    return MsgProcessed;
  }

  virtual void OnChannelClose() override {
    MOZ_RELEASE_ASSERT(mSide == ipc::ChildSide);
    BeginShutdown();
  }

  virtual void OnChannelError() override { BeginShutdown(); }
};

// Protocol forwarding messages from the UI process to the recording process.
// Messages are received on the main thread, and forwarded on the forwarding
// message loop thread.
static MiddlemanProtocol* gChildProtocol;

// Protocol forwarding messages from the recording process to the UI process.
// Messages are received on the forwarding message loop thread, and forwarded
// on the main thread.
static MiddlemanProtocol* gParentProtocol;

void MaybeHandleForwardedMessages() {
  if (gParentProtocol) {
    gParentProtocol->MaybeSendSyncMessage();
  }
  ProcessMainThreadRunnables();
}

ipc::MessageChannel* ChannelToUIProcess() {
  return gChildProtocol->GetIPCChannel();
}

// Message loop for forwarding messages between the parent process and a
// recording process.
static MessageLoop* gForwardingMessageLoop;

static bool gParentProtocolOpened = false;

// Main routine for the forwarding message loop thread.
static void ForwardingMessageLoopMain(void*) {
  MessageLoop messageLoop;
  gForwardingMessageLoop = &messageLoop;

  gChildProtocol->mOppositeMessageLoop = gForwardingMessageLoop;

  gParentProtocol->Open(
      gRecordingProcess->TakeChannel(),
      base::GetProcId(gRecordingProcess->GetChildProcessHandle()));

  // Notify the main thread that we have finished initialization.
  {
    MonitorAutoLock lock(*gMonitor);
    gParentProtocolOpened = true;
    gMonitor->Notify();
  }

  messageLoop.Run();
}

void InitializeForwarding() {
  gChildProtocol = new MiddlemanProtocol(ipc::ChildSide);

  if (gProcessKind == ProcessKind::MiddlemanRecording) {
    gParentProtocol = new MiddlemanProtocol(ipc::ParentSide);
    gParentProtocol->mOpposite = gChildProtocol;
    gChildProtocol->mOpposite = gParentProtocol;

    gParentProtocol->mOppositeMessageLoop = MainThreadMessageLoop();

    if (!PR_CreateThread(PR_USER_THREAD, ForwardingMessageLoopMain, nullptr,
                         PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD,
                         PR_JOINABLE_THREAD, 0)) {
      MOZ_CRASH("parent::Initialize");
    }

    // Wait for the forwarding message loop thread to finish initialization.
    MonitorAutoLock lock(*gMonitor);
    while (!gParentProtocolOpened) {
      gMonitor->Wait();
    }
  }
}

}  // namespace parent
}  // namespace recordreplay
}  // namespace mozilla
