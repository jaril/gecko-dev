/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file has the logic which the middleman process uses to send messages to
// the UI process with painting data from the child process.

#include "ParentInternal.h"

#include "chrome/common/mach_ipc_mac.h"
#include "jsapi.h"  // JSAutoRealm
#include "js/ArrayBuffer.h"  // JS::{DetachArrayBuffer,NewArrayBufferWithUserOwnedContents}
#include "js/RootingAPI.h"  // JS::Rooted
#include "js/Value.h"       // JS::{,Object}Value
#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/layers/CompositorBridgeChild.h"
#include "mozilla/layers/ImageDataSerializer.h"
#include "mozilla/layers/LayerTransactionChild.h"
#include "mozilla/layers/PTextureChild.h"
#include "nsImportModule.h"
#include "nsStringStream.h"
#include "rrIGraphics.h"
#include "ImageOps.h"

#include <mach/mach_vm.h>
#include <unistd.h>

namespace mozilla {
namespace recordreplay {
namespace parent {

// Graphics memory buffer shared with all child processes.
void* gGraphicsMemory;

static mach_port_t gGraphicsPort;
static ReceivePort* gGraphicsReceiver;

void InitializeGraphicsMemory() {
  mach_vm_address_t address;
  kern_return_t kr = mach_vm_allocate(mach_task_self(), &address,
                                      GraphicsMemorySize, VM_FLAGS_ANYWHERE);
  MOZ_RELEASE_ASSERT(kr == KERN_SUCCESS);

  memory_object_size_t memoryObjectSize = GraphicsMemorySize;
  kr = mach_make_memory_entry_64(mach_task_self(), &memoryObjectSize, address,
                                 VM_PROT_DEFAULT, &gGraphicsPort,
                                 MACH_PORT_NULL);
  MOZ_RELEASE_ASSERT(kr == KERN_SUCCESS);
  MOZ_RELEASE_ASSERT(memoryObjectSize == GraphicsMemorySize);

  gGraphicsMemory = (void*)address;
  gGraphicsReceiver =
      new ReceivePort(nsPrintfCString("RecordReplay.%d", getpid()).get());
}

void SendGraphicsMemoryToChild() {
  MachReceiveMessage handshakeMessage;
  kern_return_t kr = gGraphicsReceiver->WaitForMessage(&handshakeMessage, 0);
  MOZ_RELEASE_ASSERT(kr == KERN_SUCCESS);

  MOZ_RELEASE_ASSERT(handshakeMessage.GetMessageID() ==
                     GraphicsHandshakeMessageId);
  mach_port_t childPort = handshakeMessage.GetTranslatedPort(0);
  MOZ_RELEASE_ASSERT(childPort != MACH_PORT_NULL);

  MachSendMessage message(GraphicsMemoryMessageId);
  message.AddDescriptor(
      MachMsgPortDescriptor(gGraphicsPort, MACH_MSG_TYPE_COPY_SEND));

  MachPortSender sender(childPort);
  kr = sender.SendMessage(message, 1000);
  MOZ_RELEASE_ASSERT(kr == KERN_SUCCESS);
}

// Global object for the sandbox used to paint graphics data in this process.
static StaticRefPtr<rrIGraphics> gGraphics;

static void InitGraphicsSandbox() {
  MOZ_RELEASE_ASSERT(!gGraphics);

  nsCOMPtr<rrIGraphics> graphics =
      do_ImportModule("resource://devtools/server/actors/replay/graphics.js");
  gGraphics = graphics.forget();
  ClearOnShutdown(&gGraphics);

  MOZ_RELEASE_ASSERT(gGraphics);
}

// Buffer used to transform graphics memory, if necessary.
static void* gBufferMemory;

// Last ArrayBuffer object used for rendering;
static JS::PersistentRootedObject* gLastBuffer;

static void UpdateMiddlemanCanvas(size_t aWidth, size_t aHeight, size_t aStride,
                                  void* aData, const nsACString& aOptions) {
  // Make sure the width and height are appropriately sized.
  CheckedInt<size_t> scaledWidth = CheckedInt<size_t>(aWidth) * 4;
  CheckedInt<size_t> scaledHeight = CheckedInt<size_t>(aHeight) * aStride;
  MOZ_RELEASE_ASSERT(scaledWidth.isValid() && scaledWidth.value() <= aStride);
  MOZ_RELEASE_ASSERT(scaledHeight.isValid() &&
                     scaledHeight.value() <= GraphicsMemorySize);

  // Get memory which we can pass to the graphics module to store in a canvas.
  // Use the shared memory buffer directly, unless we need to transform the
  // data due to extra memory in each row of the data which the child process
  // sent us.
  MOZ_RELEASE_ASSERT(aData);
  void* memory = aData;
  if (aStride != aWidth * 4) {
    if (!gBufferMemory) {
      gBufferMemory = malloc(GraphicsMemorySize);
    }
    memory = gBufferMemory;
    for (size_t y = 0; y < aHeight; y++) {
      char* src = (char*)aData + y * aStride;
      char* dst = (char*)gBufferMemory + y * aWidth * 4;
      memcpy(dst, src, aWidth * 4);
    }
  }

  AutoSafeJSContext cx;
  JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());

  // The graphics module always needs the last buffer to be usable. Now that we
  // are doing a new render, the last buffer can be detached from its contents.
  if (gLastBuffer && *gLastBuffer) {
    MOZ_ALWAYS_TRUE(JS::DetachArrayBuffer(cx, *gLastBuffer));
    *gLastBuffer = nullptr;
  }

  // Create an ArrayBuffer whose contents are the externally-provided |memory|.
  JS::Rooted<JSObject*> bufferObject(cx);
  bufferObject =
      JS::NewArrayBufferWithUserOwnedContents(cx, aWidth * aHeight * 4, memory);
  MOZ_RELEASE_ASSERT(bufferObject);

  JS::Rooted<JS::Value> buffer(cx, JS::ObjectValue(*bufferObject));

  // Call into the graphics module to update the canvas it manages.
  if (NS_FAILED(gGraphics->UpdateCanvas(buffer, aWidth, aHeight, aOptions))) {
    MOZ_CRASH("UpdateMiddlemanCanvas");
  }

  if (!gLastBuffer) {
    gLastBuffer = new JS::PersistentRootedObject(cx);
  }
  *gLastBuffer = bufferObject;
}

void UpdateGraphicsAfterPaint(const PaintMessage& aMsg) {
  MOZ_RELEASE_ASSERT(NS_IsMainThread());

  if (!aMsg.mWidth || !aMsg.mHeight) {
    return;
  }

  // Make sure there is a sandbox which is running the graphics JS module.
  if (!gGraphics) {
    InitGraphicsSandbox();
  }

  size_t stride = layers::ImageDataSerializer::ComputeRGBStride(gSurfaceFormat,
                                                                aMsg.mWidth);
  UpdateMiddlemanCanvas(aMsg.mWidth, aMsg.mHeight, stride, gGraphicsMemory,
                        nsCString());
}

}  // namespace parent
}  // namespace recordreplay
}  // namespace mozilla
