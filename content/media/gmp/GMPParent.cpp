/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GMPParent.h"
#include "nsComponentManagerUtils.h"
#include "nsComponentManagerUtils.h"
#include "nsIInputStream.h"
#include "nsILineInputStream.h"
#include "nsNetUtil.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsThreadUtils.h"
#include "nsIRunnable.h"
#include "mozIGeckoMediaPluginService.h"
#include "mozilla/unused.h"
#include "nsIObserverService.h"
#include "mtransport/runnable_utils.h"

#include "mozilla/dom/CrashReporterParent.h"
using mozilla::dom::CrashReporterParent;

#ifdef MOZ_CRASHREPORTER
using CrashReporter::AnnotationTable;
using CrashReporter::GetIDFromMinidump;
#endif

namespace mozilla {
namespace gmp {

GMPParent::GMPParent()
  : mState(GMPStateNotLoaded)
  , mProcess(nullptr)
{
}

GMPParent::~GMPParent()
{
}

nsresult
GMPParent::Init(nsIFile* aPluginDir)
{
  MOZ_ASSERT(aPluginDir);
  MOZ_ASSERT(GMPThread() == NS_GetCurrentThread());

  mDirectory = aPluginDir;

  nsAutoString leafname;
  nsresult rv = aPluginDir->GetLeafName(leafname);
  if (NS_FAILED(rv)) {
    return rv;
  }

  MOZ_ASSERT(leafname.Length() > 4);
  mName = Substring(leafname, 4);

  return ReadGMPMetaData();
}

nsresult
GMPParent::LoadProcess()
{
  MOZ_ASSERT(mDirectory, "Plugin directory cannot be NULL!");
  MOZ_ASSERT(GMPThread() == NS_GetCurrentThread());
  MOZ_ASSERT(mState == GMPStateNotLoaded);

  nsAutoCString path;
  if (NS_FAILED(mDirectory->GetNativePath(path))) {
    return NS_ERROR_FAILURE;
  }

  mProcess = new GMPProcessParent(path.get());
  if (!mProcess->Launch(30 * 1000)) {
    mProcess->Delete();
    mProcess = nullptr;
    return NS_ERROR_FAILURE;
  }

  bool opened = Open(mProcess->GetChannel(), mProcess->GetChildProcessHandle());
  if (!opened) {
    mProcess->Delete();
    mProcess = nullptr;
    return NS_ERROR_FAILURE;
  }

  mState = GMPStateLoaded;

  return NS_OK;
}

void
GMPParent::CloseIfUnused()
{
  MOZ_ASSERT(GMPThread() == NS_GetCurrentThread());

  if ((mState == GMPStateLoaded ||
       mState == GMPStateUnloading) &&
      mVideoDecoders.IsEmpty() &&
      mVideoEncoders.IsEmpty()) {
    Shutdown();
  }
}

void
GMPParent::CloseActive()
{
  if (mState == GMPStateLoaded) {
    mState = GMPStateUnloading;
  }

  // Invalidate and remove any remaining API objects.
  for (uint32_t i = mVideoDecoders.Length(); i > 0; i--) {
    mVideoDecoders[i - 1]->DecodingComplete();
  }

  // Invalidate and remove any remaining API objects.
  for (uint32_t i = mVideoEncoders.Length(); i > 0; i--) {
    mVideoEncoders[i - 1]->EncodingComplete();
  }

  // Note: the shutdown of the codecs is async!  don't kill
  // the plugin-container until they're all safely shut down via
  // CloseIfUnused();
  CloseIfUnused();
}

void
GMPParent::Shutdown()
{
  MOZ_ASSERT(GMPThread() == NS_GetCurrentThread());

  MOZ_ASSERT(mVideoDecoders.IsEmpty() && mVideoEncoders.IsEmpty());
  if (mState == GMPStateNotLoaded || mState == GMPStateClosing) {
    return;
  }

  mState = GMPStateClosing;
  Close();
  DeleteProcess();
  MOZ_ASSERT(mState == GMPStateNotLoaded);
}

void
GMPParent::DeleteProcess()
{
  MOZ_ASSERT(mState == GMPStateClosing);
  mProcess->Delete();
  mProcess = nullptr;
  mState = GMPStateNotLoaded;
}

void
GMPParent::VideoDecoderDestroyed(GMPVideoDecoderParent* aDecoder)
{
  MOZ_ASSERT(GMPThread() == NS_GetCurrentThread());

  // If the constructor fails, we'll get called before it's added
  unused << NS_WARN_IF(!mVideoDecoders.RemoveElement(aDecoder));

  if (mVideoDecoders.IsEmpty() &&
      mVideoEncoders.IsEmpty()) {
    // Recv__delete__ is on the stack, don't potentially destroy the top-level actor
    // until after this has completed.
    nsCOMPtr<nsIRunnable> event = NS_NewRunnableMethod(this, &GMPParent::CloseIfUnused);
    NS_DispatchToCurrentThread(event);
  }
}

void
GMPParent::VideoEncoderDestroyed(GMPVideoEncoderParent* aEncoder)
{
  MOZ_ASSERT(GMPThread() == NS_GetCurrentThread());

  // If the constructor fails, we'll get called before it's added
  unused << NS_WARN_IF(!mVideoEncoders.RemoveElement(aEncoder));

  if (mVideoDecoders.IsEmpty() &&
      mVideoEncoders.IsEmpty()) {
    // Recv__delete__ is on the stack, don't potentially destroy the top-level actor
    // until after this has completed.
    nsCOMPtr<nsIRunnable> event = NS_NewRunnableMethod(this, &GMPParent::CloseIfUnused);
    NS_DispatchToCurrentThread(event);
  }
}

GMPState
GMPParent::State() const
{
  return mState;
}

#ifdef DEBUG
nsIThread*
GMPParent::GMPThread()
{
  if (!mGMPThread) {
    nsCOMPtr<mozIGeckoMediaPluginService> mps = do_GetService("@mozilla.org/gecko-media-plugin-service;1");
    MOZ_ASSERT(mps);
    if (!mps) {
      return nullptr;
    }
    // Not really safe if we just grab to the mGMPThread, as we don't know
    // what thread we're running on and other threads may be trying to
    // access this without locks!  However, debug only, and primary failure
    // mode outside of compiler-helped TSAN is a leak.  But better would be
    // to use swap() under a lock.
    mps->GetThread(getter_AddRefs(mGMPThread));
    MOZ_ASSERT(mGMPThread);
  }

  return mGMPThread;
}
#endif

bool
GMPParent::SupportsAPI(const nsCString& aAPI, const nsCString& aTag)
{
  for (uint32_t i = 0; i < mCapabilities.Length(); i++) {
    if (!mCapabilities[i]->mAPIName.Equals(aAPI)) {
      continue;
    }
    nsTArray<nsCString>& tags = mCapabilities[i]->mAPITags;
    for (uint32_t j = 0; j < tags.Length(); j++) {
      if (tags[j].Equals(aTag)) {
        return true;
      }
    }
  }
  return false;
}

bool
GMPParent::EnsureProcessLoaded()
{
  if (mState == GMPStateLoaded) {
    return true;
  }
  if (mState == GMPStateClosing ||
      mState == GMPStateUnloading) {
    return false;
  }

  nsresult rv = LoadProcess();

  return NS_SUCCEEDED(rv);
}

nsresult
GMPParent::GetGMPVideoDecoder(GMPVideoDecoderParent** aGMPVD)
{
  MOZ_ASSERT(GMPThread() == NS_GetCurrentThread());

  if (!EnsureProcessLoaded()) {
    return NS_ERROR_FAILURE;
  }

  // returned with one anonymous AddRef that locks it until Destroy
  PGMPVideoDecoderParent* pvdp = SendPGMPVideoDecoderConstructor();
  if (!pvdp) {
    return NS_ERROR_FAILURE;
  }
  GMPVideoDecoderParent *vdp = static_cast<GMPVideoDecoderParent*>(pvdp);
  *aGMPVD = vdp;
  mVideoDecoders.AppendElement(vdp);

  return NS_OK;
}

nsresult
GMPParent::GetGMPVideoEncoder(GMPVideoEncoderParent** aGMPVE)
{
  MOZ_ASSERT(GMPThread() == NS_GetCurrentThread());

  if (!EnsureProcessLoaded()) {
    return NS_ERROR_FAILURE;
  }

  // returned with one anonymous AddRef that locks it until Destroy
  PGMPVideoEncoderParent* pvep = SendPGMPVideoEncoderConstructor();
  if (!pvep) {
    return NS_ERROR_FAILURE;
  }
  GMPVideoEncoderParent *vep = static_cast<GMPVideoEncoderParent*>(pvep);
  *aGMPVE = vep;
  mVideoEncoders.AppendElement(vep);

  return NS_OK;
}

#ifdef MOZ_CRASHREPORTER
void
GMPParent::WriteExtraDataForMinidump(CrashReporter::AnnotationTable& notes)
{
  notes.Put(NS_LITERAL_CSTRING("GMPPlugin"), NS_LITERAL_CSTRING("1"));
  notes.Put(NS_LITERAL_CSTRING("PluginFilename"),
                               NS_ConvertUTF16toUTF8(mName));
  notes.Put(NS_LITERAL_CSTRING("PluginName"), mDisplayName);
  notes.Put(NS_LITERAL_CSTRING("PluginVersion"), mVersion);
}

void
GMPParent::GetCrashID(nsString& aResult)
{
  CrashReporterParent* cr = nullptr;
  if (ManagedPCrashReporterParent().Length() > 0) {
    cr = static_cast<CrashReporterParent*>(ManagedPCrashReporterParent()[0]);
  }
  if (NS_WARN_IF(!cr)) {
    return;
  }

  AnnotationTable notes(4);
  WriteExtraDataForMinidump(notes);
  nsCOMPtr<nsIFile> dumpFile;
  TakeMinidump(getter_AddRefs(dumpFile), nullptr);
  if (!dumpFile) {
    NS_WARNING("GMP crash without crash report");
    return;
  }
  GetIDFromMinidump(dumpFile, aResult);
  cr->GenerateCrashReportForMinidump(dumpFile, &notes);
}
#endif

static void
GMPNotifyObservers(nsAString& aData)
{
  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    nsString temp(aData);
    obs->NotifyObservers(nullptr, "gmp-plugin-crash", temp.get());
  }
}

void
GMPParent::ActorDestroy(ActorDestroyReason aWhy)
{
#ifdef MOZ_CRASHREPORTER
  if (AbnormalShutdown == aWhy) {
    nsString dumpID;
    GetCrashID(dumpID);
    nsString id;
    // use the parent address to identify it
    // We could use any unique-to-the-parent value
    id.AppendInt(reinterpret_cast<uint64_t>(this));
    id.Append(NS_LITERAL_STRING(" "));
    AppendUTF8toUTF16(mDisplayName, id);
    id.Append(NS_LITERAL_STRING(" "));
    id.Append(dumpID);

    // NotifyObservers is mainthread-only
    NS_DispatchToMainThread(WrapRunnableNM(&GMPNotifyObservers, id),
                            NS_DISPATCH_NORMAL);
  }
#endif
  // warn us off trying to close again
  mState = GMPStateClosing;
  CloseActive();

  // Normal Shutdown() will delete the process on unwind.
  if (AbnormalShutdown == aWhy) {
    NS_DispatchToCurrentThread(NS_NewRunnableMethod(this, &GMPParent::DeleteProcess));
  }
}

mozilla::dom::PCrashReporterParent*
GMPParent::AllocPCrashReporterParent(const NativeThreadId& aThread)
{
#ifndef MOZ_CRASHREPORTER
  MOZ_ASSERT(false, "Should only be sent if crash reporting is enabled.");
#endif
  CrashReporterParent* cr = new CrashReporterParent();
  cr->SetChildData(aThread, GeckoProcessType_GMPlugin);
  return cr;
}

bool
GMPParent::DeallocPCrashReporterParent(PCrashReporterParent* aCrashReporter)
{
  delete aCrashReporter;
  return true;
}

PGMPVideoDecoderParent*
GMPParent::AllocPGMPVideoDecoderParent()
{
  GMPVideoDecoderParent* vdp = new GMPVideoDecoderParent(this);
  NS_ADDREF(vdp);
  return vdp;
}

bool
GMPParent::DeallocPGMPVideoDecoderParent(PGMPVideoDecoderParent* aActor)
{
  GMPVideoDecoderParent* vdp = static_cast<GMPVideoDecoderParent*>(aActor);
  NS_RELEASE(vdp);
  return true;
}

PGMPVideoEncoderParent*
GMPParent::AllocPGMPVideoEncoderParent()
{
  GMPVideoEncoderParent* vep = new GMPVideoEncoderParent(this);
  NS_ADDREF(vep);
  return vep;
}

bool
GMPParent::DeallocPGMPVideoEncoderParent(PGMPVideoEncoderParent* aActor)
{
  GMPVideoEncoderParent* vep = static_cast<GMPVideoEncoderParent*>(aActor);
  NS_RELEASE(vep);
  return true;
}

nsresult
ParseNextRecord(nsILineInputStream* aLineInputStream,
                const nsCString& aPrefix,
                nsCString& aResult,
                bool& aMoreLines)
{
  nsAutoCString record;
  nsresult rv = aLineInputStream->ReadLine(record, &aMoreLines);
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (record.Length() <= aPrefix.Length() ||
      !Substring(record, 0, aPrefix.Length()).Equals(aPrefix)) {
    return NS_ERROR_FAILURE;
  }

  aResult = Substring(record, aPrefix.Length());
  aResult.Trim("\b\t\r\n ");

  return NS_OK;
}

nsresult
GMPParent::ReadGMPMetaData()
{
  MOZ_ASSERT(mDirectory, "Plugin directory cannot be NULL!");
  MOZ_ASSERT(!mName.IsEmpty(), "Plugin mName cannot be empty!");

  nsCOMPtr<nsIFile> infoFile;
  nsresult rv = mDirectory->Clone(getter_AddRefs(infoFile));
  if (NS_FAILED(rv)) {
    return rv;
  }
  infoFile->AppendRelativePath(mName + NS_LITERAL_STRING(".info"));

  nsCOMPtr<nsIInputStream> inputStream;
  rv = NS_NewLocalFileInputStream(getter_AddRefs(inputStream), infoFile);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsILineInputStream> lineInputStream = do_QueryInterface(inputStream, &rv);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCString value;
  bool moreLines = false;

  // 'Name:' record
  nsCString prefix = NS_LITERAL_CSTRING("Name:");
  rv = ParseNextRecord(lineInputStream, prefix, value, moreLines);
  if (NS_FAILED(rv)) {
    return rv;
  }
  if (value.IsEmpty()) {
    // Not OK for name to be empty. Must have one non-whitespace character.
    return NS_ERROR_FAILURE;
  }
  mDisplayName = value;

  // 'Description:' record
  if (!moreLines) {
    return NS_ERROR_FAILURE;
  }
  prefix = NS_LITERAL_CSTRING("Description:");
  rv = ParseNextRecord(lineInputStream, prefix, value, moreLines);
  if (NS_FAILED(rv)) {
    return rv;
  }
  mDescription = value;

  // 'Version:' record
  if (!moreLines) {
    return NS_ERROR_FAILURE;
  }
  prefix = NS_LITERAL_CSTRING("Version:");
  rv = ParseNextRecord(lineInputStream, prefix, value, moreLines);
  if (NS_FAILED(rv)) {
    return rv;
  }
  mVersion = value;

  // 'Capability:' record
  if (!moreLines) {
    return NS_ERROR_FAILURE;
  }
  prefix = NS_LITERAL_CSTRING("APIs:");
  rv = ParseNextRecord(lineInputStream, prefix, value, moreLines);
  if (NS_FAILED(rv)) {
    return rv;
  }
  nsCCharSeparatedTokenizer apiTokens(value, ',');
  while (apiTokens.hasMoreTokens()) {
    nsAutoCString api(apiTokens.nextToken());
    api.StripWhitespace();
    if (api.IsEmpty()) {
      continue;
    }

    int32_t tagsStart = api.FindChar('[');
    if (tagsStart == 0) {
      // Not allowed to be the first character.
      // API name must be at least one character.
      continue;
    }

    auto cap = new GMPCapability();
    if (tagsStart == -1) {
      // No tags.
      cap->mAPIName.Assign(api);
    } else {
      auto tagsEnd = api.FindChar(']');
      if (tagsEnd == -1 || tagsEnd < tagsStart) {
        // Invalid syntax, skip whole capability.
        delete cap;
        continue;
      }

      cap->mAPIName.Assign(Substring(api, 0, tagsStart));

      if ((tagsEnd - tagsStart) > 1) {
        const nsDependentCSubstring ts(Substring(api, tagsStart + 1, tagsEnd - tagsStart - 1));
        nsCCharSeparatedTokenizer tagTokens(ts, ':');
        while (tagTokens.hasMoreTokens()) {
          const nsDependentCSubstring tag(tagTokens.nextToken());
          cap->mAPITags.AppendElement(tag);
        }
      }
    }

    mCapabilities.AppendElement(cap);
  }

  if (mCapabilities.IsEmpty()) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

bool
GMPParent::CanBeSharedCrossOrigin() const
{
  return mOrigin.IsEmpty();
}

bool
GMPParent::CanBeUsedFrom(const nsAString& aOrigin) const
{
  return (mOrigin.IsEmpty() && State() == GMPStateNotLoaded) ||
         mOrigin.Equals(aOrigin);
}

void
GMPParent::SetOrigin(const nsAString& aOrigin)
{
  MOZ_ASSERT(!aOrigin.IsEmpty());
  MOZ_ASSERT(CanBeUsedFrom(aOrigin));
  mOrigin = aOrigin;
}

} // namespace gmp
} // namespace mozilla
