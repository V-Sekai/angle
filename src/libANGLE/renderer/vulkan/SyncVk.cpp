//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// SyncVk.cpp:
//    Implements the class methods for SyncVk.
//

#include "libANGLE/renderer/vulkan/SyncVk.h"

#include "common/debug.h"
#include "libANGLE/Context.h"
#include "libANGLE/Display.h"
#include "libANGLE/renderer/vulkan/ContextVk.h"
#include "libANGLE/renderer/vulkan/DisplayVk.h"

#if !defined(ANGLE_PLATFORM_WINDOWS)
#    include <unistd.h>
#else
#    include <io.h>
#endif

namespace rx
{
namespace vk
{
SyncHelper::SyncHelper() {}

SyncHelper::~SyncHelper() {}

void SyncHelper::releaseToRenderer(RendererVk *renderer)
{
    renderer->collectGarbageAndReinit(&mUse, &mEvent);
    // TODO: https://issuetracker.google.com/170312581 - Currently just stalling on worker thread
    // here to try and avoid race condition. If this works, need some alternate solution
    if (renderer->getFeatures().enableCommandProcessingThread.enabled)
    {
        renderer->waitForCommandProcessorIdle(nullptr);
    }
    mFence.reset(renderer->getDevice());
}

angle::Result SyncHelper::initialize(ContextVk *contextVk)
{
    ASSERT(!mEvent.valid());

    RendererVk *renderer = contextVk->getRenderer();
    VkDevice device      = renderer->getDevice();

    VkEventCreateInfo eventCreateInfo = {};
    eventCreateInfo.sType             = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    eventCreateInfo.flags             = 0;

    DeviceScoped<Event> event(device);
    ANGLE_VK_TRY(contextVk, event.get().init(device, eventCreateInfo));
    // TODO: https://issuetracker.google.com/170312581 - For now wait for worker thread to finish
    // then get next fence from renderer
    if (contextVk->getRenderer()->getFeatures().enableCommandProcessingThread.enabled)
    {
        contextVk->getRenderer()->waitForCommandProcessorIdle(contextVk);
        ANGLE_TRY(contextVk->getRenderer()->getNextSubmitFence(&mFence, false));
    }
    else
    {
        ANGLE_TRY(contextVk->getNextSubmitFence(&mFence));
    }

    mEvent = event.release();

    vk::CommandBuffer &commandBuffer = contextVk->getOutsideRenderPassCommandBuffer();
    commandBuffer.setEvent(mEvent.getHandle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    retain(&contextVk->getResourceUseList());

    contextVk->onSyncHelperInitialize();

    return angle::Result::Continue;
}

angle::Result SyncHelper::clientWait(Context *context,
                                     ContextVk *contextVk,
                                     bool flushCommands,
                                     uint64_t timeout,
                                     VkResult *outResult)
{
    RendererVk *renderer = context->getRenderer();

    // If the event is already set, don't wait
    bool alreadySignaled = false;
    ANGLE_TRY(getStatus(context, &alreadySignaled));
    if (alreadySignaled)
    {
        *outResult = VK_EVENT_SET;
        return angle::Result::Continue;
    }

    // If timeout is zero, there's no need to wait, so return timeout already.
    if (timeout == 0)
    {
        *outResult = VK_TIMEOUT;
        return angle::Result::Continue;
    }

    if (flushCommands && contextVk)
    {
        ANGLE_TRY(contextVk->flushImpl(nullptr));
    }

    // If we are using worker need to wait for the commands to be issued before waiting on the
    // fence.
    if (renderer->getFeatures().enableCommandProcessingThread.enabled)
    {
        renderer->waitForCommandProcessorIdle(contextVk);
    }

    // Wait on the fence that's expected to be signaled on the first vkQueueSubmit after
    // `initialize` was called. The first fence is the fence created to signal this sync.
    ASSERT(mFence.get().valid());
    // TODO: https://issuetracker.google.com/170312581 - Wait could be command to worker
    VkResult status = mFence.get().wait(renderer->getDevice(), timeout);

    // Check for errors, but don't consider timeout as such.
    if (status != VK_TIMEOUT)
    {
        ANGLE_VK_TRY(context, status);
    }

    *outResult = status;
    return angle::Result::Continue;
}

angle::Result SyncHelper::serverWait(ContextVk *contextVk)
{
    vk::CommandBuffer &commandBuffer = contextVk->getOutsideRenderPassCommandBuffer();
    commandBuffer.waitEvents(1, mEvent.ptr(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, nullptr, 0, nullptr, 0,
                             nullptr);
    retain(&contextVk->getResourceUseList());
    return angle::Result::Continue;
}

angle::Result SyncHelper::getStatus(Context *context, bool *signaled) const
{
    VkResult result = mEvent.getStatus(context->getDevice());
    if (result != VK_EVENT_SET && result != VK_EVENT_RESET)
    {
        ANGLE_VK_TRY(context, result);
    }
    *signaled = (result == VK_EVENT_SET);
    return angle::Result::Continue;
}

SyncHelperNativeFence::SyncHelperNativeFence() : mNativeFenceFd(kInvalidFenceFd) {}

SyncHelperNativeFence::~SyncHelperNativeFence()
{
    if (mNativeFenceFd != kInvalidFenceFd)
    {
        close(mNativeFenceFd);
    }
}

void SyncHelperNativeFence::releaseToRenderer(RendererVk *renderer)
{
    renderer->collectGarbageAndReinit(&mUse, &mFenceWithFd);
}

// Note: Having mFenceWithFd hold the FD, so that ownership is with ICD. Meanwhile store a dup
// of FD in SyncHelperNativeFence for further reference, i.e. dup of FD. Any call to clientWait
// or serverWait will ensure the FD or dup of FD goes to application or ICD. At release, above
// it's Garbage collected/destroyed. Otherwise can't time when to close(fd);
angle::Result SyncHelperNativeFence::initializeWithFd(ContextVk *contextVk, int inFd)
{
    ASSERT(inFd >= kInvalidFenceFd);

    RendererVk *renderer = contextVk->getRenderer();
    VkDevice device      = renderer->getDevice();

    DeviceScoped<vk::Fence> fence(device);

    VkExportFenceCreateInfo exportCreateInfo = {};
    exportCreateInfo.sType                   = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
    exportCreateInfo.pNext                   = nullptr;
    exportCreateInfo.handleTypes             = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR;

    // Create fenceInfo base.
    VkFenceCreateInfo fenceCreateInfo = {};
    fenceCreateInfo.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.flags             = 0;
    fenceCreateInfo.pNext             = &exportCreateInfo;

    // Initialize/create a VkFence handle
    ANGLE_VK_TRY(contextVk, fence.get().init(device, fenceCreateInfo));

    int importFenceFd = kInvalidFenceFd;
    // If valid FD provided by application - import it to fence.
    if (inFd > kInvalidFenceFd)
    {
        importFenceFd = inFd;
    }
    // If invalid FD provided by application - create one with fence.
    else
    {
        /*
          Spec: "When a fence sync object is created or when an EGL native fence sync
          object is created with the EGL_SYNC_NATIVE_FENCE_FD_ANDROID attribute set to
          EGL_NO_NATIVE_FENCE_FD_ANDROID, eglCreateSyncKHR also inserts a fence command
          into the command stream of the bound client API's current context and associates it
          with the newly created sync object.
        */
        // Flush first because the fence comes after current pending set of commands.
        ANGLE_TRY(contextVk->flushImpl(nullptr));

        retain(&contextVk->getResourceUseList());

        if (renderer->getFeatures().enableCommandProcessingThread.enabled)
        {
            CommandProcessorTask oneOffQueueSubmit;
            oneOffQueueSubmit.initOneOffQueueSubmit(VK_NULL_HANDLE, contextVk->getPriority(),
                                                    &fence.get());
            renderer->queueCommand(contextVk, &oneOffQueueSubmit);
            // TODO: https://issuetracker.google.com/170312581 - wait for now
            renderer->waitForCommandProcessorIdle(contextVk);
        }
        else
        {
            Serial serialOut;
            VkSubmitInfo submitInfo = {};
            submitInfo.sType        = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            ANGLE_TRY(renderer->queueSubmit(contextVk, contextVk->getPriority(), submitInfo,
                                            nullptr, &fence.get(), &serialOut));
        }

        VkFenceGetFdInfoKHR fenceGetFdInfo = {};
        fenceGetFdInfo.sType               = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
        fenceGetFdInfo.fence               = fence.get().getHandle();
        fenceGetFdInfo.handleType          = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
        ANGLE_VK_TRY(contextVk, fence.get().exportFd(device, fenceGetFdInfo, &importFenceFd));
    }

    // Spec: Importing a fence payload from a file descriptor transfers ownership of the file
    // descriptor from the application to the Vulkan implementation. The application must not
    // perform any operations on the file descriptor after a successful import.

    // Make a dup of importFenceFd before tranfering ownership to created fence.
    mNativeFenceFd = dup(importFenceFd);

    // Import FD - after creating fence.
    VkImportFenceFdInfoKHR importFenceFdInfo = {};
    importFenceFdInfo.sType                  = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR;
    importFenceFdInfo.pNext                  = nullptr;
    importFenceFdInfo.fence                  = fence.get().getHandle();
    importFenceFdInfo.flags                  = VK_FENCE_IMPORT_TEMPORARY_BIT_KHR;
    importFenceFdInfo.handleType             = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    importFenceFdInfo.fd                     = importFenceFd;

    ANGLE_VK_TRY(contextVk, fence.get().importFd(device, importFenceFdInfo));
    mFenceWithFd = fence.release();
    retain(&contextVk->getResourceUseList());

    return angle::Result::Continue;
}

angle::Result SyncHelperNativeFence::clientWait(Context *context,
                                                ContextVk *contextVk,
                                                bool flushCommands,
                                                uint64_t timeout,
                                                VkResult *outResult)
{
    RendererVk *renderer = context->getRenderer();

    // If already signaled, don't wait
    bool alreadySignaled = false;
    ANGLE_TRY(getStatus(context, &alreadySignaled));
    if (alreadySignaled)
    {
        *outResult = VK_SUCCESS;
        return angle::Result::Continue;
    }

    // If timeout is zero, there's no need to wait, so return timeout already.
    if (timeout == 0)
    {
        *outResult = VK_TIMEOUT;
        return angle::Result::Continue;
    }

    if (flushCommands && contextVk)
    {
        ANGLE_TRY(contextVk->flushImpl(nullptr));
    }

    // If we are using worker need to wait for the commands to be issued before waiting on the
    // fence.
    if (contextVk->getRenderer()->getFeatures().asynchronousCommandProcessing.enabled)
    {
        contextVk->getRenderer()->waitForCommandProcessorIdle(contextVk);
    }

    // Wait for mFenceWithFd to be signaled.
    VkResult status = mFenceWithFd.wait(renderer->getDevice(), timeout);

    // Check for errors, but don't consider timeout as such.
    if (status != VK_TIMEOUT)
    {
        ANGLE_VK_TRY(context, status);
    }

    *outResult = status;
    return angle::Result::Continue;
}

angle::Result SyncHelperNativeFence::serverWait(ContextVk *contextVk)
{
    if (!mFenceWithFd.valid())
    {
        return angle::Result::Stop;
    }

    RendererVk *renderer = contextVk->getRenderer();
    VkDevice device      = renderer->getDevice();

    DeviceScoped<Semaphore> waitSemaphore(device);
    // Wait semaphore for next vkQueueSubmit().
    // Create a Semaphore with imported fenceFd.
    ANGLE_VK_TRY(contextVk, waitSemaphore.get().init(device));

    VkImportSemaphoreFdInfoKHR importFdInfo = {};
    importFdInfo.sType                      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    importFdInfo.semaphore                  = waitSemaphore.get().getHandle();
    importFdInfo.flags                      = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT_KHR;
    importFdInfo.handleType                 = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
    importFdInfo.fd                         = dup(mNativeFenceFd);
    ANGLE_VK_TRY(contextVk, waitSemaphore.get().importFd(device, importFdInfo));

    // Flush current work, block after current pending commands.
    ANGLE_TRY(contextVk->flushImpl(nullptr));

    // Add semaphore to next submit job.
    contextVk->addWaitSemaphore(waitSemaphore.get().getHandle(),
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    contextVk->addGarbage(&waitSemaphore.get());  // This releases the handle.
    return angle::Result::Continue;
}

angle::Result SyncHelperNativeFence::getStatus(Context *context, bool *signaled) const
{
    VkResult result = mFenceWithFd.getStatus(context->getDevice());
    if (result != VK_SUCCESS && result != VK_NOT_READY)
    {
        ANGLE_VK_TRY(context, result);
    }
    *signaled = (result == VK_SUCCESS);
    return angle::Result::Continue;
}

angle::Result SyncHelperNativeFence::dupNativeFenceFD(Context *context, int *fdOut) const
{
    if (!mFenceWithFd.valid() || mNativeFenceFd == kInvalidFenceFd)
    {
        return angle::Result::Stop;
    }

    *fdOut = dup(mNativeFenceFd);

    return angle::Result::Continue;
}

}  // namespace vk

SyncVk::SyncVk() : SyncImpl() {}

SyncVk::~SyncVk() {}

void SyncVk::onDestroy(const gl::Context *context)
{
    mSyncHelper.releaseToRenderer(vk::GetImpl(context)->getRenderer());
}

angle::Result SyncVk::set(const gl::Context *context, GLenum condition, GLbitfield flags)
{
    ASSERT(condition == GL_SYNC_GPU_COMMANDS_COMPLETE);
    ASSERT(flags == 0);

    return mSyncHelper.initialize(vk::GetImpl(context));
}

angle::Result SyncVk::clientWait(const gl::Context *context,
                                 GLbitfield flags,
                                 GLuint64 timeout,
                                 GLenum *outResult)
{
    ContextVk *contextVk = vk::GetImpl(context);

    ASSERT((flags & ~GL_SYNC_FLUSH_COMMANDS_BIT) == 0);

    bool flush = (flags & GL_SYNC_FLUSH_COMMANDS_BIT) != 0;
    VkResult result;

    ANGLE_TRY(mSyncHelper.clientWait(contextVk, contextVk, flush, static_cast<uint64_t>(timeout),
                                     &result));

    switch (result)
    {
        case VK_EVENT_SET:
            *outResult = GL_ALREADY_SIGNALED;
            return angle::Result::Continue;

        case VK_SUCCESS:
            *outResult = GL_CONDITION_SATISFIED;
            return angle::Result::Continue;

        case VK_TIMEOUT:
            *outResult = GL_TIMEOUT_EXPIRED;
            return angle::Result::Incomplete;

        default:
            UNREACHABLE();
            *outResult = GL_WAIT_FAILED;
            return angle::Result::Stop;
    }
}

angle::Result SyncVk::serverWait(const gl::Context *context, GLbitfield flags, GLuint64 timeout)
{
    ASSERT(flags == 0);
    ASSERT(timeout == GL_TIMEOUT_IGNORED);

    ContextVk *contextVk = vk::GetImpl(context);
    return mSyncHelper.serverWait(contextVk);
}

angle::Result SyncVk::getStatus(const gl::Context *context, GLint *outResult)
{
    bool signaled = false;
    ANGLE_TRY(mSyncHelper.getStatus(vk::GetImpl(context), &signaled));

    *outResult = signaled ? GL_SIGNALED : GL_UNSIGNALED;
    return angle::Result::Continue;
}

EGLSyncVk::EGLSyncVk(const egl::AttributeMap &attribs)
    : EGLSyncImpl(), mSyncHelper(nullptr), mAttribs(attribs)
{}

EGLSyncVk::~EGLSyncVk()
{
    SafeDelete<vk::SyncHelper>(mSyncHelper);
}

void EGLSyncVk::onDestroy(const egl::Display *display)
{
    mSyncHelper->releaseToRenderer(vk::GetImpl(display)->getRenderer());
}

egl::Error EGLSyncVk::initialize(const egl::Display *display,
                                 const gl::Context *context,
                                 EGLenum type)
{
    ASSERT(context != nullptr);
    mType = type;

    switch (type)
    {
        case EGL_SYNC_FENCE_KHR:
            ASSERT(mAttribs.isEmpty());
            mSyncHelper = new vk::SyncHelper();
            if (mSyncHelper->initialize(vk::GetImpl(context)) == angle::Result::Stop)
            {
                return egl::Error(EGL_BAD_ALLOC, "eglCreateSyncKHR failed to create sync object");
            }
            return egl::NoError();
        case EGL_SYNC_NATIVE_FENCE_ANDROID:
        {
            vk::SyncHelperNativeFence *syncHelper = new vk::SyncHelperNativeFence();
            mSyncHelper                           = syncHelper;
            int nativeFd = static_cast<EGLint>(mAttribs.getAsInt(EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
                                                                 EGL_NO_NATIVE_FENCE_FD_ANDROID));
            return angle::ToEGL(syncHelper->initializeWithFd(vk::GetImpl(context), nativeFd),
                                vk::GetImpl(display), EGL_BAD_ALLOC);
        }
        default:
            UNREACHABLE();
            return egl::Error(EGL_BAD_ALLOC);
    }
}

egl::Error EGLSyncVk::clientWait(const egl::Display *display,
                                 const gl::Context *context,
                                 EGLint flags,
                                 EGLTime timeout,
                                 EGLint *outResult)
{
    ASSERT((flags & ~EGL_SYNC_FLUSH_COMMANDS_BIT_KHR) == 0);

    bool flush = (flags & EGL_SYNC_FLUSH_COMMANDS_BIT_KHR) != 0;
    VkResult result;

    ContextVk *contextVk = context ? vk::GetImpl(context) : nullptr;
    if (mSyncHelper->clientWait(vk::GetImpl(display), contextVk, flush,
                                static_cast<uint64_t>(timeout), &result) == angle::Result::Stop)
    {
        return egl::Error(EGL_BAD_ALLOC);
    }

    switch (result)
    {
        case VK_EVENT_SET:
            // fall through.  EGL doesn't differentiate between event being already set, or set
            // before timeout.
        case VK_SUCCESS:
            *outResult = EGL_CONDITION_SATISFIED_KHR;
            return egl::NoError();

        case VK_TIMEOUT:
            *outResult = EGL_TIMEOUT_EXPIRED_KHR;
            return egl::NoError();

        default:
            UNREACHABLE();
            *outResult = EGL_FALSE;
            return egl::Error(EGL_BAD_ALLOC);
    }
}

egl::Error EGLSyncVk::serverWait(const egl::Display *display,
                                 const gl::Context *context,
                                 EGLint flags)
{
    // Server wait requires a valid bound context.
    ASSERT(context);

    // No flags are currently implemented.
    ASSERT(flags == 0);

    DisplayVk *displayVk = vk::GetImpl(display);
    ContextVk *contextVk = vk::GetImpl(context);

    return angle::ToEGL(mSyncHelper->serverWait(contextVk), displayVk, EGL_BAD_ALLOC);
}

egl::Error EGLSyncVk::getStatus(const egl::Display *display, EGLint *outStatus)
{
    bool signaled = false;
    if (mSyncHelper->getStatus(vk::GetImpl(display), &signaled) == angle::Result::Stop)
    {
        return egl::Error(EGL_BAD_ALLOC);
    }

    *outStatus = signaled ? EGL_SIGNALED_KHR : EGL_UNSIGNALED_KHR;
    return egl::NoError();
}

egl::Error EGLSyncVk::dupNativeFenceFD(const egl::Display *display, EGLint *fdOut) const
{
    switch (mType)
    {
        case EGL_SYNC_NATIVE_FENCE_ANDROID:
            return angle::ToEGL(mSyncHelper->dupNativeFenceFD(vk::GetImpl(display), fdOut),
                                vk::GetImpl(display), EGL_BAD_PARAMETER);
        default:
            return egl::EglBadDisplay();
    }
}

}  // namespace rx
