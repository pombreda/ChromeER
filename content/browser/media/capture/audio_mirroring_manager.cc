// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/media/capture/audio_mirroring_manager.h"

#include "base/lazy_instance.h"

namespace content {

namespace {

base::LazyInstance<AudioMirroringManager>::Leaky g_audio_mirroring_manager =
    LAZY_INSTANCE_INITIALIZER;

}  // namespace

// static
AudioMirroringManager* AudioMirroringManager::GetInstance() {
  return g_audio_mirroring_manager.Pointer();
}

AudioMirroringManager::AudioMirroringManager() {
  // Only *after* construction, check that AudioMirroringManager is being
  // invoked on the same single thread.
  thread_checker_.DetachFromThread();
}

AudioMirroringManager::~AudioMirroringManager() {}

void AudioMirroringManager::AddDiverter(
    int render_process_id, int render_view_id, Diverter* diverter) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(diverter);

  // DCHECK(diverter not already in diverters_ under any key)
#ifndef NDEBUG
  for (DiverterMap::const_iterator it = diverters_.begin();
       it != diverters_.end(); ++it) {
    DCHECK_NE(diverter, it->second);
  }
#endif

  // Add the diverter to the set of active diverters.
  const Target target(render_process_id, render_view_id);
  diverters_.insert(std::make_pair(target, diverter));

  // If a mirroring session is active, start diverting the audio stream
  // immediately.
  SessionMap::iterator session_it = sessions_.find(target);
  if (session_it != sessions_.end()) {
    diverter->StartDiverting(
        session_it->second->AddInput(diverter->GetAudioParameters()));
  }
}

void AudioMirroringManager::RemoveDiverter(
    int render_process_id, int render_view_id, Diverter* diverter) {
  DCHECK(thread_checker_.CalledOnValidThread());

  // Stop diverting the audio stream if a mirroring session is active.
  const Target target(render_process_id, render_view_id);
  SessionMap::iterator session_it = sessions_.find(target);
  if (session_it != sessions_.end())
    diverter->StopDiverting();

  // Remove the diverter from the set of active diverters.
  for (DiverterMap::iterator it = diverters_.lower_bound(target);
       it != diverters_.end() && it->first == target; ++it) {
    if (it->second == diverter) {
      diverters_.erase(it);
      break;
    }
  }
}

void AudioMirroringManager::StartMirroring(
    int render_process_id, int render_view_id,
    MirroringDestination* destination) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(destination);

  // Insert an entry into the set of active mirroring sessions.  If a mirroring
  // session is already active for |render_process_id| + |render_view_id|,
  // replace the entry.
  const Target target(render_process_id, render_view_id);
  SessionMap::iterator session_it = sessions_.find(target);
  MirroringDestination* old_destination;
  if (session_it == sessions_.end()) {
    old_destination = NULL;
    sessions_.insert(std::make_pair(target, destination));

    DVLOG(1) << "Start mirroring render_process_id:render_view_id="
             << render_process_id << ':' << render_view_id
             << " --> MirroringDestination@" << destination;
  } else {
    old_destination = session_it->second;
    session_it->second = destination;

    DVLOG(1) << "Switch mirroring of render_process_id:render_view_id="
             << render_process_id << ':' << render_view_id
             << "  MirroringDestination@" << old_destination
             << " --> MirroringDestination@" << destination;
  }

  // Divert audio streams coming from |target| to |destination|.  If streams
  // were already diverted to the |old_destination|, remove them.
  for (DiverterMap::iterator it = diverters_.lower_bound(target);
       it != diverters_.end() && it->first == target; ++it) {
    Diverter* const diverter = it->second;
    if (old_destination)
      diverter->StopDiverting();
    diverter->StartDiverting(
        destination->AddInput(diverter->GetAudioParameters()));
  }
}

void AudioMirroringManager::StopMirroring(
    int render_process_id, int render_view_id,
    MirroringDestination* destination) {
  DCHECK(thread_checker_.CalledOnValidThread());

  // Stop mirroring if there is an active session *and* the destination
  // matches.
  const Target target(render_process_id, render_view_id);
  SessionMap::iterator session_it = sessions_.find(target);
  if (session_it == sessions_.end() || destination != session_it->second)
    return;

  DVLOG(1) << "Stop mirroring render_process_id:render_view_id="
           << render_process_id << ':' << render_view_id
           << " --> MirroringDestination@" << destination;

  // Stop diverting each audio stream in the mirroring session being stopped.
  for (DiverterMap::iterator it = diverters_.lower_bound(target);
       it != diverters_.end() && it->first == target; ++it) {
    it->second->StopDiverting();
  }

  // Remove the entry from the set of active mirroring sessions.
  sessions_.erase(session_it);
}

}  // namespace content
