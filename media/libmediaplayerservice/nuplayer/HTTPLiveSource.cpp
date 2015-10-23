/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "NU-HTTPLiveSource"
#include <utils/Log.h>

#include "HTTPLiveSource.h"

#include "AnotherPacketSource.h"
#include "LiveDataSource.h"

#include <media/IMediaHTTPService.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDefs.h>

namespace android {

NuPlayer::HTTPLiveSource::HTTPLiveSource(
        const sp<AMessage> &notify,
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers,
        interruptcallback pfunc)
    : Source(notify),
      mHTTPService(httpService),
      mURL(url),
      mBuffering(false),
      mFlags(0),
      mFinalResult(OK),
      mOffset(0),
      mFetchSubtitleDataGeneration(0),
      mFetchMetaDataGeneration(0),
      mHasMetadata(false),
      mMetadataSelected(false),
      mInterruptCallback(pfunc) {
    if (headers) {
        mExtraHeaders = *headers;

        ssize_t index =
            mExtraHeaders.indexOfKey(String8("x-hide-urls-from-log"));

        if (index >= 0) {
            mFlags |= kFlagIncognito;

            mExtraHeaders.removeItemsAt(index);
        }
    }
}

NuPlayer::HTTPLiveSource::~HTTPLiveSource() {
    ALOGI("[%s:%d] start !", __FUNCTION__, __LINE__);
    if (mLiveSession != NULL) {
        mLiveSession->disconnect();

        mLiveLooper->unregisterHandler(mLiveSession->id());
        mLiveLooper->unregisterHandler(id());
        mLiveLooper->stop();

        mLiveSession.clear();
        mLiveLooper.clear();
    }
    ALOGI("[%s:%d] end !", __FUNCTION__, __LINE__);
}

void NuPlayer::HTTPLiveSource::prepareAsync() {
    if (mLiveLooper == NULL) {
        mLiveLooper = new ALooper;
        mLiveLooper->setName("http live");
        mLiveLooper->start();

        mLiveLooper->registerHandler(this);
    }

    sp<AMessage> notify = new AMessage(kWhatSessionNotify, this);

    mLiveSession = new LiveSession(
            notify,
            (mFlags & kFlagIncognito) ? LiveSession::kFlagIncognito : 0,
            mHTTPService/*,
            mInterruptCallback*/);

    //mLiveSession->setParentThreadId(mParentThreadId);

    mLiveLooper->registerHandler(mLiveSession);

    mLiveSession->connectAsync(
            mURL.c_str(), mExtraHeaders.isEmpty() ? NULL : &mExtraHeaders);

}

void NuPlayer::HTTPLiveSource::start() {
}

void NuPlayer::HTTPLiveSource::setParentThreadId(android_thread_id_t thread_id) {
    mParentThreadId = thread_id;
}

sp<AMessage> NuPlayer::HTTPLiveSource::getFormat(bool audio) {
    if (mLiveSession == NULL) {
        return NULL;
    }

    sp<AMessage> format;
    status_t err = mLiveSession->getStreamFormat(
            audio ? LiveSession::STREAMTYPE_AUDIO
                  : LiveSession::STREAMTYPE_VIDEO,
            &format);

    if (err != OK) {
        return NULL;
    }

    return format;
}

status_t NuPlayer::HTTPLiveSource::feedMoreTSData() {
    return OK;
}

status_t NuPlayer::HTTPLiveSource::dequeueAccessUnit(
        bool audio, sp<ABuffer> *accessUnit) {
    if (mBuffering) {
        //if (!mLiveSession->haveSufficientDataOnAVTracks()) {
        //    return -EWOULDBLOCK;
        //}
        mBuffering = false;
        sp<AMessage> notify = dupNotify();
        notify->setInt32("what", kWhatBufferingEnd);
        notify->post();
        notify->setInt32("what", kWhatResumeOnBufferingEnd);
        notify->post();
        ALOGI("HTTPLiveSource buffering end!\n");
    }

    bool needBuffering = false;
    status_t finalResult = 0;//mLiveSession->hasBufferAvailable(audio, &needBuffering);
    if (needBuffering) {
        mBuffering = true;
        sp<AMessage> notify = dupNotify();
        notify->setInt32("what", kWhatPauseOnBufferingStart);
        notify->post();
        notify->setInt32("what", kWhatBufferingStart);
        notify->post();
        ALOGI("HTTPLiveSource buffering start!\n");
        return finalResult;
    }

    return mLiveSession->dequeueAccessUnit(
            audio ? LiveSession::STREAMTYPE_AUDIO
                  : LiveSession::STREAMTYPE_VIDEO,
            accessUnit);
}

status_t NuPlayer::HTTPLiveSource::getDuration(int64_t *durationUs) {
    return mLiveSession->getDuration(durationUs);
}

size_t NuPlayer::HTTPLiveSource::getTrackCount() const {
    return mLiveSession->getTrackCount();
}

sp<AMessage> NuPlayer::HTTPLiveSource::getTrackInfo(size_t trackIndex) const {
    return mLiveSession->getTrackInfo(trackIndex);
}

ssize_t NuPlayer::HTTPLiveSource::getSelectedTrack(media_track_type type) const {
    if (mLiveSession == NULL) {
        return -1;
    } else if (type == MEDIA_TRACK_TYPE_METADATA) {
        // MEDIA_TRACK_TYPE_METADATA is always last track
        // mMetadataSelected can only be true when mHasMetadata is true
        return mMetadataSelected ? (mLiveSession->getTrackCount() - 1) : -1;
    } else {
        return mLiveSession->getSelectedTrack(type);
    }
}

status_t NuPlayer::HTTPLiveSource::selectTrack(size_t trackIndex, bool select, int64_t /*timeUs*/) {
    if (mLiveSession == NULL) {
        return INVALID_OPERATION;
    }

    status_t err = INVALID_OPERATION;
    bool postFetchMsg = false, isSub = false;
    if (!mHasMetadata || trackIndex != mLiveSession->getTrackCount() - 1) {
        err = mLiveSession->selectTrack(trackIndex, select);
        postFetchMsg = select;
        isSub = true;
    } else {
        // metadata track; i.e. (mHasMetadata && trackIndex == mLiveSession->getTrackCount() - 1)
        if (mMetadataSelected && !select) {
            err = OK;
        } else if (!mMetadataSelected && select) {
            postFetchMsg = true;
            err = OK;
        } else {
            err = BAD_VALUE; // behave as LiveSession::selectTrack
        }

        mMetadataSelected = select;
    }

#if 0
    if (err == OK) {
        int32_t &generation = isSub ? mFetchSubtitleDataGeneration : mFetchMetaDataGeneration;
        generation++;
        if (postFetchMsg) {
            int32_t what = isSub ? kWhatFetchSubtitleData : kWhatFetchMetaData;
            sp<AMessage> msg = new AMessage(what, this);
            msg->setInt32("generation", generation);
            msg->post();
        }
    }
#endif
    // LiveSession::selectTrack returns BAD_VALUE when selecting the currently
    // selected track, or unselecting a non-selected track. In this case it's an
    // no-op so we return OK.
    return (err == OK || err == BAD_VALUE) ? (status_t)OK : err;
}

status_t NuPlayer::HTTPLiveSource::seekTo(int64_t seekTimeUs) {
    return mLiveSession->seekTo(seekTimeUs);
}

void NuPlayer::HTTPLiveSource::pollForRawData(
        const sp<AMessage> &msg, int32_t currentGeneration,
        LiveSession::StreamType fetchType, int32_t pushWhat) {

    int32_t generation;
    CHECK(msg->findInt32("generation", &generation));

    if (generation != currentGeneration) {
        return;
    }

    sp<ABuffer> buffer;
    while (mLiveSession->dequeueAccessUnit(fetchType, &buffer) == OK) {

        sp<AMessage> notify = dupNotify();
        notify->setInt32("what", pushWhat);
        notify->setBuffer("buffer", buffer);

        int64_t timeUs, baseUs, delayUs;
        CHECK(buffer->meta()->findInt64("baseUs", &baseUs));
        CHECK(buffer->meta()->findInt64("timeUs", &timeUs));
        delayUs = baseUs + timeUs - ALooper::GetNowUs();

        if (fetchType == LiveSession::STREAMTYPE_SUBTITLES) {
            notify->post();
            msg->post(delayUs > 0ll ? delayUs : 0ll);
            return;
        } else if (fetchType == LiveSession::STREAMTYPE_METADATA) {
            if (delayUs < -1000000ll) { // 1 second
                continue;
            }
            notify->post();
            // push all currently available metadata buffers in each invocation of pollForRawData
            // continue;
        } else {
            TRESPASS();
        }
    }

    // try again in 1 second
    msg->post(1000000ll);
}

void NuPlayer::HTTPLiveSource::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatSessionNotify:
        {
            onSessionNotify(msg);
            break;
        }

        case kWhatFetchSubtitleData:
        {
            pollForRawData(
                    msg, mFetchSubtitleDataGeneration,
                    /* fetch */ LiveSession::STREAMTYPE_SUBTITLES,
                    /* push */ kWhatSubtitleData);

            break;
        }

        case kWhatFetchMetaData:
        {
            if (!mMetadataSelected) {
                break;
            }

            pollForRawData(
                    msg, mFetchMetaDataGeneration,
                    /* fetch */ LiveSession::STREAMTYPE_METADATA,
                    /* push */ kWhatTimedMetaData);

            break;
        }

        default:
            Source::onMessageReceived(msg);
            break;
    }
}

void NuPlayer::HTTPLiveSource::onSessionNotify(const sp<AMessage> &msg) {
    int32_t what;
    CHECK(msg->findInt32("what", &what));
    ALOGI("session notify : %d\n", what);

    switch (what) {
        case LiveSession::kWhatPrepared:
        {
            // notify the current size here if we have it, otherwise report an initial size of (0,0)
            ALOGI("session notify prepared!\n");

            sp<AMessage> notify = dupNotify();
            notify->setInt32("what", kWhatSourceReady);
            notify->setInt32("err", 0);
            notify->post();

            sp<AMessage> format = getFormat(false /* audio */);
            int32_t width;
            int32_t height;
            if (format != NULL &&
                    format->findInt32("width", &width) && format->findInt32("height", &height)) {
                notifyVideoSizeChanged(format);
            } else {
                notifyVideoSizeChanged();
            }

            uint32_t flags = FLAG_CAN_PAUSE;
            if (mLiveSession->isSeekable()) {
                flags |= FLAG_CAN_SEEK;
                flags |= FLAG_CAN_SEEK_BACKWARD;
                flags |= FLAG_CAN_SEEK_FORWARD;
            }

            if (mLiveSession->hasDynamicDuration()) {
                flags |= FLAG_DYNAMIC_DURATION;
            }

            notifyFlagsChanged(flags);

            notifyPrepared();
            break;
        }

        case LiveSession::kWhatPreparationFailed:
        {
            ALOGI("session notify preparation failed!\n");
            status_t err;
            CHECK(msg->findInt32("err", &err));

            notifyPrepared(err);
            break;
        }

        case LiveSession::kWhatStreamsChanged:
        {
            ALOGI("session notify streams changed!\n");
            uint32_t changedMask;
            CHECK(msg->findInt32(
                        "changedMask", (int32_t *)&changedMask));

            bool audio = changedMask & LiveSession::STREAMTYPE_AUDIO;
            bool video = changedMask & LiveSession::STREAMTYPE_VIDEO;

            sp<AMessage> reply;
            CHECK(msg->findMessage("reply", &reply));

            sp<AMessage> notify = dupNotify();
            notify->setInt32("what", kWhatQueueDecoderShutdown);
            notify->setInt32("audio", audio);
            notify->setInt32("video", video);
            notify->setMessage("reply", reply);
            notify->post();
            break;
        }

        case LiveSession::kWhatBufferingStart:
        {
            sp<AMessage> notify = dupNotify();
            notify->setInt32("what", kWhatPauseOnBufferingStart);
            notify->post();
            break;
        }

        case LiveSession::kWhatBufferingEnd:
        {
            sp<AMessage> notify = dupNotify();
            notify->setInt32("what", kWhatResumeOnBufferingEnd);
            notify->post();
            break;
        }


        case LiveSession::kWhatBufferingUpdate:
        {
            sp<AMessage> notify = dupNotify();
            int32_t percentage;
            CHECK(msg->findInt32("percentage", &percentage));
            notify->setInt32("what", kWhatBufferingUpdate);
            notify->setInt32("percentage", percentage);
            notify->post();
            break;
        }

        case LiveSession::kWhatMetadataDetected:
        {
            if (!mHasMetadata) {
                mHasMetadata = true;

                sp<AMessage> notify = dupNotify();
                // notification without buffer triggers MEDIA_INFO_METADATA_UPDATE
                notify->setInt32("what", kWhatTimedMetaData);
                notify->post();
            }
            break;
        }

        case LiveSession::kWhatError:
        {
            int32_t err;
            CHECK(msg->findInt32("err", &err));
            if (err == ERROR_UNSUPPORTED) {
                sp<AMessage> notify = dupNotify();
                notify->setInt32("what", kWhatSourceReady);
                notify->setInt32("err", 1);
                notify->post();
            }
            break;
        }

        case LiveSession::kWhatSourceReady:
        {
            sp<AMessage> notify = dupNotify();
            notify->setInt32("what", kWhatSourceReady);
            int32_t err;
            CHECK(msg->findInt32("err", &err));
            notify->setInt32("err", err);
            notify->post();
            break;
        }

        default:
            TRESPASS();
    }
}

}  // namespace android

