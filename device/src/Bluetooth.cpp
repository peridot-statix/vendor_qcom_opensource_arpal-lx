/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "PAL: Bluetooth"
#include "Bluetooth.h"
#include "ResourceManager.h"
#include "PayloadBuilder.h"
#include "Stream.h"
#include "Session.h"
#include "SessionAlsaUtils.h"
#include "Device.h"
#include "kvh2xml.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cutils/properties.h>
#include <sstream>
#include <string>
#include <regex>

#define BT_IPC_SOURCE_LIB "btaudio_offload_if.so"
#define BT_IPC_SINK_LIB "libbthost_if_sink.so"
#define PARAM_ID_RESET_PLACEHOLDER_MODULE          0x08001173
#define MIXER_SET_FEEDBACK_CHANNEL "BT set feedback channel"

Bluetooth::Bluetooth(struct pal_device *device, std::shared_ptr<ResourceManager> Rm)
    : Device(device, Rm),
      codecFormat(CODEC_TYPE_INVALID),
      codecInfo(NULL),
      isAbrEnabled(false),
      isConfigured(false),
      isLC3MonoModeOn(false),
      isTwsMonoModeOn(false),
      isDummySink(false),
      abrRefCnt(0)
{
}

Bluetooth::~Bluetooth()
{
}

int Bluetooth::updateDeviceMetadata()
{
    int ret = 0;
    std::string backEndName;
    std::vector <std::pair<int, int>> keyVector;

    switch(deviceAttr.id) {
    case PAL_DEVICE_IN_BLUETOOTH_A2DP:
    case PAL_DEVICE_OUT_BLUETOOTH_A2DP:
        if (deviceAttr.id == PAL_DEVICE_OUT_BLUETOOTH_A2DP)
            keyVector.push_back(std::make_pair(DEVICERX, BT_RX));
        else
            keyVector.push_back(std::make_pair(DEVICETX, BT_TX));
        keyVector.push_back(std::make_pair(BT_PROFILE, A2DP));

        switch (codecFormat) {
        case CODEC_TYPE_LDAC:
            PAL_INFO(LOG_TAG, "Setting BT_FORMAT = LDAC");
            keyVector.push_back(std::make_pair(BT_FORMAT, LDAC));
            break;
        case CODEC_TYPE_APTX_AD:
            PAL_INFO(LOG_TAG, "Setting BT_FORMAT = APTX_ADAPTIVE");
            keyVector.push_back(std::make_pair(BT_FORMAT, APTX_ADAPTIVE));
            break;
        case CODEC_TYPE_LC3:
            PAL_INFO(LOG_TAG, "Setting BT_FORMAT = LC3");
            keyVector.push_back(std::make_pair(BT_FORMAT, LC3));
            break;
        case CODEC_TYPE_AAC:
            if (isAbrEnabled) {
                PAL_INFO(LOG_TAG, "Setting BT_FORMAT = AAC_ABR");
                keyVector.push_back(std::make_pair(BT_FORMAT, AAC_ABR));
                break;
            }
            [[fallthrough]];
        case CODEC_TYPE_SBC:
        case CODEC_TYPE_CELT:
        case CODEC_TYPE_APTX:
        case CODEC_TYPE_APTX_HD:
        case CODEC_TYPE_APTX_DUAL_MONO:
        default:
            PAL_INFO(LOG_TAG, "Setting BT_FORMAT = GENERIC, codecFormat = 0x%x", codecFormat);
            keyVector.push_back(std::make_pair(BT_FORMAT, GENERIC));
            break;
        }
        break;
    case PAL_DEVICE_OUT_BLUETOOTH_SCO:
    case PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET:
        if (deviceAttr.id == PAL_DEVICE_OUT_BLUETOOTH_SCO)
            keyVector.push_back(std::make_pair(DEVICERX, BT_RX));
        else
            keyVector.push_back(std::make_pair(DEVICETX, BT_TX));

        keyVector.push_back(std::make_pair(BT_PROFILE, SCO));
        switch (codecFormat) {
        case CODEC_TYPE_APTX_AD_SPEECH:
            PAL_INFO(LOG_TAG, "Setting BT_FORMAT = SWB");
            keyVector.push_back(std::make_pair(BT_FORMAT, SWB));
            break;
        case CODEC_TYPE_LC3:
            PAL_INFO(LOG_TAG, "Setting BT_FORMAT = LC3");
            keyVector.push_back(std::make_pair(BT_FORMAT, LC3));
            break;
        default:
            break;
        }
        break;
    default:
        return -EINVAL;
    }
    rm->getBackendName(deviceAttr.id, backEndName);
    ret = SessionAlsaUtils::setDeviceMetadata(rm, backEndName, keyVector);
    return ret;
}

void Bluetooth::updateDeviceAttributes()
{
    deviceAttr.config.sample_rate = codecConfig.sample_rate;

    switch (codecFormat) {
    case CODEC_TYPE_AAC:
    case CODEC_TYPE_SBC:
        if (codecType == DEC &&
            (codecConfig.sample_rate == 44100 ||
             codecConfig.sample_rate == 48000))
            deviceAttr.config.sample_rate = codecConfig.sample_rate * 2;
        break;
    case CODEC_TYPE_LDAC:
    case CODEC_TYPE_APTX_AD:
        if (codecType == ENC &&
            (codecConfig.sample_rate == 44100 ||
             codecConfig.sample_rate == 48000))
        deviceAttr.config.sample_rate = codecConfig.sample_rate * 2;
        break;
    case CODEC_TYPE_APTX_AD_SPEECH:
        deviceAttr.config.sample_rate = SAMPLINGRATE_96K;
        deviceAttr.config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_COMPRESSED;
        break;
    case CODEC_TYPE_LC3:
        deviceAttr.config.sample_rate = SAMPLINGRATE_96K;
        deviceAttr.config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_COMPRESSED;
        break;
    default:
        break;
    }
}

bool Bluetooth::isPlaceholderEncoder()
{
    switch (codecFormat) {
        case CODEC_TYPE_LDAC:
        case CODEC_TYPE_APTX_AD:
        case CODEC_TYPE_APTX_AD_SPEECH:
        case CODEC_TYPE_LC3:
            return false;
        case CODEC_TYPE_AAC:
            return isAbrEnabled ? false : true;
        default:
            return true;
    }
}

int Bluetooth::getPluginPayload(void **libHandle, bt_codec_t **btCodec,
              bt_enc_payload_t **out_buf, codec_type codecType)
{
    std::string lib_path;
    open_fn_t plugin_open_fn = NULL;
    int status = 0;
    bt_codec_t *codec = NULL;
    void *handle = NULL;

    lib_path = rm->getBtCodecLib(codecFormat, (codecType == ENC ? "enc" : "dec"));
    if (lib_path.empty()) {
        PAL_ERR(LOG_TAG, "fail to get BT codec library");
        return -ENOSYS;
    }

    handle = dlopen(lib_path.c_str(), RTLD_NOW);
    if (handle == NULL) {
        PAL_ERR(LOG_TAG, "failed to dlopen lib %s", lib_path.c_str());
        return -EINVAL;
    }

    dlerror();
    plugin_open_fn = (open_fn_t)dlsym(handle, "plugin_open");
    if (!plugin_open_fn) {
        PAL_ERR(LOG_TAG, "dlsym to open fn failed, err = '%s'", dlerror());
        status = -EINVAL;
        goto error;
    }

    status = plugin_open_fn(&codec, codecFormat, codecType);
    if (status) {
        PAL_ERR(LOG_TAG, "failed to open plugin %d", status);
        goto error;
    }

    status = codec->plugin_populate_payload(codec, codecInfo, (void **)out_buf);
    if (status != 0) {
        PAL_ERR(LOG_TAG, "fail to pack the encoder config %d", status);
        goto error;
    }
    *btCodec = codec;
    *libHandle = handle;
    goto done;

error:
    if (codec)
        codec->close_plugin(codec);

    if (handle)
        dlclose(handle);
done:
    return status;
}

int Bluetooth::configureA2dpEncoderDecoder()
{
    int status = 0, i;
    Stream *stream = NULL;
    Session *session = NULL;
    std::vector<Stream*> activestreams;
    bt_enc_payload_t *out_buf = NULL;
    PayloadBuilder* builder = new PayloadBuilder();
    std::string backEndName;
    uint8_t* paramData = NULL;
    size_t paramSize = 0;
    uint32_t codecTagId = 0;
    uint32_t miid = 0, ratMiid = 0, copMiid = 0, cnvMiid = 0;
    std::shared_ptr<Device> dev = nullptr;
    uint32_t num_payloads;

    isConfigured = false;
    rm->getBackendName(deviceAttr.id, backEndName);

    dev = Device::getInstance(&deviceAttr, rm);
    status = rm->getActiveStream_l(dev, activestreams);
    if ((0 != status) || (activestreams.size() == 0)) {
        PAL_ERR(LOG_TAG, "no active stream available");
        status = -EINVAL;
        goto error;
    }
    stream = static_cast<Stream *>(activestreams[0]);
    stream->getAssociatedSession(&session);
    PAL_INFO(LOG_TAG, "choose BT codec format %x", codecFormat);


    /* Retrieve plugin library from resource manager.
     * Map to interested symbols.
     */
    status = getPluginPayload(&pluginHandler, &pluginCodec, &out_buf, codecType);
    if (status) {
        PAL_ERR(LOG_TAG, "failed to payload from plugin");
        goto error;
    }

    codecConfig.sample_rate = out_buf->sample_rate;
    codecConfig.bit_width = out_buf->bit_format;
    codecConfig.ch_info.channels = out_buf->channel_count;

    isAbrEnabled = out_buf->is_abr_enabled;

    /* Update Device sampleRate based on encoder config */
    updateDeviceAttributes();

    codecTagId = (codecType == ENC ? BT_PLACEHOLDER_ENCODER : BT_PLACEHOLDER_DECODER);
    status = session->getMIID(backEndName.c_str(), codecTagId, &miid);
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to get tag info %x, status = %d", codecTagId, status);
        goto error;
    }

    if (isPlaceholderEncoder()) {
        PAL_DBG(LOG_TAG, "Resetting placeholder module");
        builder->payloadCustomParam(&paramData, &paramSize, NULL, 0,
                                    miid, PARAM_ID_RESET_PLACEHOLDER_MODULE);
        if (!paramData) {
            PAL_ERR(LOG_TAG, "Failed to populateAPMHeader");
            status = -ENOMEM;
            goto error;
        }
        dev->updateCustomPayload(paramData, paramSize);
        free(paramData);
        paramData = NULL;
        paramSize = 0;
    }

    /* BT Encoder & Decoder Module Configuration */
    num_payloads = out_buf->num_blks;
    for (i = 0; i < num_payloads; i++) {
        custom_block_t *blk = out_buf->blocks[i];
        builder->payloadCustomParam(&paramData, &paramSize,
                  (uint32_t *)blk->payload, blk->payload_sz, miid, blk->param_id);
        if (!paramData) {
            PAL_ERR(LOG_TAG, "Failed to populateAPMHeader");
            status = -ENOMEM;
            goto error;
        }
        dev->updateCustomPayload(paramData, paramSize);
        free(paramData);
        paramData = NULL;
        paramSize = 0;
    }

    /* ---------------------------------------------------------------------------
     *       |        Encoder       | PSPD MFC/RAT/PCM CNV | COP Packetizer/HW EP
     * ---------------------------------------------------------------------------
     * SBC   | E_SR = SR of encoder | Same as encoder      | SR:E_SR BW:16 CH:1
     * ------|                      |----------------------|----------------------
     * AAC   | E_CH = CH of encoder | Same as encoder      | SR:E_SR BW:16 CH:1
     * ------|                      |----------------------|----------------------
     * LDAC  | E_BW = BW of encoder | Same as encoder      | if E_SR = 44.1/48KHz
     *       |                      |                      |   SR:E_SR*2 BW:16 CH:1
     *       |                      |                      | else
     *       |                      |                      |   SR:E_SR BW:16 CH:1
     * ------|                      |----------------------|----------------------
     * APTX  |                      | Same as encoder      | SR:E_SR BW:16 CH:1
     * ------|                      |----------------------|----------------------
     * APTX  |                      | Same as encoder      | SR:E_SR BW:16 CH:1
     * HD    |                      |                      |
     * ------|                      |----------------------|----------------------
     * APTX  |                      | Same as encoder      | if E_SR = 44.1/48KHz
     * AD    |                      |                      |   SR:E_SR*2 BW:16 CH:1
     *       |                      |                      | else
     *       |                      |                      |   SR:E_SR BW:16 CH:1
     * ------|----------------------|----------------------|----------------------
     * LC3   | E_SR = SR of encoder | Same as encoder      | SR:96KHz BW:16 CH:1
     *       | E_CH = CH of encoder |                      |
     *       | E_BW = 24            |                      |
     * ---------------------------------------------------------------------------
     * APTX      | E_SR = 32KHz     | Same as encoder      | SR:96KHz BW:16 CH:1
     * AD Speech | E_CH = 1         |                      |
     *           | E_BW = 16        |                      |
     * ---------------------------------------------------------------------------
     * LC3       | E_SR = SR of encoder | Same as encoder  | SR:96KHz BW:16 CH:1
     * Voice     | E_CH = CH of encoder |                  |
     *           | E_BW = 24            |                  |
     * --------------------------------------------------------------------------- */
    if (codecFormat == CODEC_TYPE_APTX_AD_SPEECH) {
        PAL_DBG(LOG_TAG, "Skip the rest of static configurations coming from ACDB");
        goto done;
    }

    if (codecFormat == CODEC_TYPE_APTX_DUAL_MONO ||
        codecFormat == CODEC_TYPE_APTX_AD) {
        builder->payloadTWSConfig(&paramData, &paramSize, miid,
                                  isTwsMonoModeOn, codecFormat);
        if (paramSize) {
            dev->updateCustomPayload(paramData, paramSize);
            free(paramData);
            paramData = NULL;
            paramSize = 0;
        } else {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "Invalid TWS param size");
            goto error;
        }
    }

    if (codecFormat == CODEC_TYPE_LC3) {
        builder->payloadLC3Config(&paramData, &paramSize, miid,
                                  isLC3MonoModeOn);
        if (paramSize) {
            dev->updateCustomPayload(paramData, paramSize);
            free(paramData);
            paramData = NULL;
            paramSize = 0;
        } else {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "Invalid LC3 param size");
            goto error;
        }

        if (codecType == DEC) {
            /* COP v2 DEPACKETIZER Module Configuration */
            status = session->getMIID(backEndName.c_str(), COP_DEPACKETIZER_V2, &copMiid);
            if (status) {
                PAL_ERR(LOG_TAG, "Failed to get tag info %x, status = %d",
                        COP_DEPACKETIZER_V2, status);
                goto error;
            }

            builder->payloadCopV2DepackConfig(&paramData, &paramSize,
                    copMiid, codecInfo, false /* StreamMapOut */);
            if (paramSize) {
                dev->updateCustomPayload(paramData, paramSize);
                delete [] paramData;
                paramData = NULL;
                paramSize = 0;
            } else {
                status = -EINVAL;
                PAL_ERR(LOG_TAG, "Invalid COPv2 module param size");
                goto error;
            }

            builder->payloadCopV2DepackConfig(&paramData, &paramSize,
                    copMiid, codecInfo, true /* StreamMapIn */);
            if (paramSize) {
                dev->updateCustomPayload(paramData, paramSize);
                delete [] paramData;
                paramData = NULL;
                paramSize = 0;
            } else {
                status = -EINVAL;
                PAL_ERR(LOG_TAG, "Invalid COPv2 module param size");
                goto error;
            }

            goto done;
        }

        /* COP v2 PACKETIZER Module Configuration */
        status = session->getMIID(backEndName.c_str(), COP_PACKETIZER_V2, &copMiid);
        if (status) {
            PAL_ERR(LOG_TAG, "Failed to get tag info %x, status = %d",
                    COP_PACKETIZER_V2, status);
            goto error;
        }

        // PARAM_ID_COP_V2_STREAM_INFO for COPv2
        builder->payloadCopV2PackConfig(&paramData, &paramSize, copMiid, codecInfo);
        if (paramSize) {
            dev->updateCustomPayload(paramData, paramSize);
            delete [] paramData;
            paramData = NULL;
            paramSize = 0;
        } else {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "Invalid COPv2 module param size");
            goto error;
        }

        // PARAM_ID_COP_PACKETIZER_OUTPUT_MEDIA_FORMAT for COPv2
        builder->payloadCopPackConfig(&paramData, &paramSize, copMiid, &deviceAttr.config);
        if (paramSize) {
            dev->updateCustomPayload(paramData, paramSize);
            delete [] paramData;
            paramData = NULL;
            paramSize = 0;
        } else {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "Invalid COP module param size");
            goto error;
        }
    } else { /* codecFormat != CODEC_TYPE_LC3 */
        // Bypass COP V0 DEPACKETIZER Module Configuration for TX path
        if (codecType == DEC)
            goto done;

        /* COP v0 PACKETIZER Module Configuration */
        status = session->getMIID(backEndName.c_str(), COP_PACKETIZER_V0, &copMiid);
        if (status) {
            PAL_ERR(LOG_TAG, "Failed to get tag info %x, status = %d",
                    COP_PACKETIZER_V0, status);
            goto error;
        }

        // PARAM_ID_COP_PACKETIZER_OUTPUT_MEDIA_FORMAT for COPv0
        builder->payloadCopPackConfig(&paramData, &paramSize, copMiid, &deviceAttr.config);
        if (paramSize) {
            dev->updateCustomPayload(paramData, paramSize);
            delete [] paramData;
            paramData = NULL;
            paramSize = 0;
        } else {
            status = -EINVAL;
            PAL_ERR(LOG_TAG, "Invalid COP module param size");
            goto error;
        }
    }

    /* RAT Module Configuration */
    status = session->getMIID(backEndName.c_str(), RAT_RENDER, &ratMiid);
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to get tag info %x, status = %d", RAT_RENDER, status);
        goto error;
    }

    builder->payloadRATConfig(&paramData, &paramSize, ratMiid, &codecConfig);
    if (paramSize) {
        dev->updateCustomPayload(paramData, paramSize);
        delete [] paramData;
        paramData = NULL;
        paramSize = 0;
    } else {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid RAT module param size");
        goto error;
    }

    /* PCM CNV Module Configuration */
    status = session->getMIID(backEndName.c_str(), BT_PCM_CONVERTER, &cnvMiid);
    if (status) {
        PAL_ERR(LOG_TAG, "Failed to get tag info %x, status = %d",
                BT_PCM_CONVERTER, status);
        goto error;
    }

    builder->payloadPcmCnvConfig(&paramData, &paramSize, cnvMiid, &codecConfig);
    if (paramSize) {
        dev->updateCustomPayload(paramData, paramSize);
        delete [] paramData;
        paramData = NULL;
        paramSize = 0;
    } else {
        status = -EINVAL;
        PAL_ERR(LOG_TAG, "Invalid PCM CNV module param size");
        goto error;
    }

done:
    isConfigured = true;

error:
    if (builder) {
       delete builder;
       builder = NULL;
    }
    return status;
}

int Bluetooth::getCodecConfig(struct pal_media_config *config)
{
    if (!config) {
        PAL_ERR(LOG_TAG, "Invalid codec config");
        return -EINVAL;
    }

    if (isConfigured) {
        ar_mem_cpy(config, sizeof(struct pal_media_config),
                &codecConfig, sizeof(struct pal_media_config));
    }

    return 0;
}

void Bluetooth::startAbr()
{
    int ret = 0, dir;
    struct pal_device fbDevice;
    struct pal_channel_info ch_info;
    struct pal_stream_attributes sAttr;
    std::string backEndName;
    std::vector <std::pair<int, int>> keyVector;
    struct pcm_config config;
    struct mixer_ctl *connectCtrl = NULL;
    struct mixer_ctl *btSetFeedbackChannelCtrl = NULL;
    struct mixer *mixerHandle = NULL;
    std::ostringstream connectCtrlName;
    unsigned int flags;
    uint32_t codecTagId = 0, miid = 0;
    void *pluginLibHandle = NULL;
    bt_codec_t *codec = NULL;
    bt_enc_payload_t *out_buf = NULL;
    custom_block_t *blk = NULL;
    uint8_t* paramData = NULL;
    size_t paramSize = 0;
    PayloadBuilder* builder = NULL;


    memset(&fbDevice, 0, sizeof(fbDevice));
    memset(&sAttr, 0, sizeof(sAttr));
    memset(&config, 0, sizeof(config));

    mAbrMutex.lock();
    if (abrRefCnt > 0) {
        abrRefCnt++;
        mAbrMutex.unlock();
        return;
    }
    /* Configure device attributes */
    ch_info.channels = CHANNELS_1;
    ch_info.ch_map[0] = PAL_CHMAP_CHANNEL_FL;

    fbDevice.config.ch_info = ch_info;
    if ((codecFormat == CODEC_TYPE_APTX_AD_SPEECH) ||
        (codecFormat == CODEC_TYPE_LC3))
        fbDevice.config.sample_rate = SAMPLINGRATE_96K;
    else
        fbDevice.config.sample_rate = SAMPLINGRATE_8K;

    fbDevice.config.bit_width = BITWIDTH_16;
    fbDevice.config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_COMPRESSED;

    if (codecType == DEC) { /* Usecase is TX, feedback device will be RX */
        if (codecFormat == CODEC_TYPE_APTX_AD_SPEECH)
            fbDevice.id = PAL_DEVICE_OUT_BLUETOOTH_SCO;
        else
            fbDevice.id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;
        dir = RX_HOSTLESS;
        flags = PCM_OUT;
        keyVector.push_back(std::make_pair(DEVICERX, BT_RX));
    } else {
        fbDevice.id = ((deviceAttr.id == PAL_DEVICE_OUT_BLUETOOTH_A2DP) ?
                       PAL_DEVICE_IN_BLUETOOTH_A2DP :
                       PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET);
        dir = TX_HOSTLESS;
        flags = PCM_IN;
        keyVector.push_back(std::make_pair(DEVICETX, BT_TX));
    }

    if (((fbDevice.id == PAL_DEVICE_IN_BLUETOOTH_A2DP) ||
        (fbDevice.id == PAL_DEVICE_OUT_BLUETOOTH_A2DP)) &&
        (codecFormat == CODEC_TYPE_LC3)) {
        keyVector.push_back(std::make_pair(BT_PROFILE, A2DP));
    }

    if (fbDevice.id == PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET ||
        fbDevice.id == PAL_DEVICE_OUT_BLUETOOTH_SCO) {
        switch (codecFormat) {
        case CODEC_TYPE_APTX_AD_SPEECH:
            keyVector.push_back(std::make_pair(BT_PROFILE, SCO));
            keyVector.push_back(std::make_pair(BT_FORMAT, SWB));
            break;
        case CODEC_TYPE_LC3:
            keyVector.push_back(std::make_pair(BT_PROFILE, SCO));
            keyVector.push_back(std::make_pair(BT_FORMAT, LC3));
            break;
        default:
            PAL_ERR(LOG_TAG, "unsupported codec format %d", codecFormat);
            break;
        }
    }

    /* Configure Device Metadata */
    rm->getBackendName(fbDevice.id, backEndName);
    ret = SessionAlsaUtils::setDeviceMetadata(rm, backEndName, keyVector);
    if (ret) {
        PAL_ERR(LOG_TAG, "setDeviceMetadata for feedback device failed");
        goto done;
    }
    ret = SessionAlsaUtils::setDeviceMediaConfig(rm, backEndName, &fbDevice);
    if (ret) {
        PAL_ERR(LOG_TAG, "setDeviceMediaConfig for feedback device failed");
        goto done;
    }

    /* Retrieve Hostless PCM device id */
    sAttr.type = PAL_STREAM_LOW_LATENCY;
    sAttr.direction = PAL_AUDIO_INPUT_OUTPUT;
    fbpcmDevIds = rm->allocateFrontEndIds(sAttr, dir);
    if (fbpcmDevIds.size() == 0) {
        PAL_ERR(LOG_TAG, "allocateFrontEndIds failed");
        ret = -ENOSYS;
        goto done;
    }
    ret = rm->getAudioMixer(&mixerHandle);
    if (ret) {
        PAL_ERR(LOG_TAG, "get mixer handle failed %d", ret);
        goto free_fe;
    }

    connectCtrlName << "PCM" << fbpcmDevIds.at(0) << " connect";
    connectCtrl = mixer_get_ctl_by_name(mixerHandle, connectCtrlName.str().data());
    if (!connectCtrl) {
        PAL_ERR(LOG_TAG, "invalid mixer control: %s", connectCtrlName.str().data());
        goto free_fe;
    }

    ret = mixer_ctl_set_enum_by_string(connectCtrl, backEndName.c_str());
    if (ret) {
        PAL_ERR(LOG_TAG, "Mixer control %s set with %s failed: %d",
                connectCtrlName.str().data(), backEndName.c_str(), ret);
        goto free_fe;
    }

    // Notify ABR usecase information to BT driver to distinguish
    // between SCO and feedback usecase
    btSetFeedbackChannelCtrl = mixer_get_ctl_by_name(mixerHandle,
                                        MIXER_SET_FEEDBACK_CHANNEL);
    if (!btSetFeedbackChannelCtrl) {
        PAL_ERR(LOG_TAG, "ERROR %s mixer control not identified",
                MIXER_SET_FEEDBACK_CHANNEL);
        goto free_fe;
    }

    if (mixer_ctl_set_value(btSetFeedbackChannelCtrl, 0, 1) != 0) {
        PAL_ERR(LOG_TAG, "Failed to set BT usecase");
        goto free_fe;
    }

    if (codecFormat == CODEC_TYPE_APTX_AD_SPEECH) {
        builder = new PayloadBuilder();

        fbDev = std::dynamic_pointer_cast<BtSco>(BtSco::getInstance(&fbDevice, rm));
        if (!fbDev) {
            PAL_ERR(LOG_TAG, "failed to get BtSco singleton object.");
            goto err_pcm_open;
        }

        if (fbDev->isConfigured == true) {
            PAL_INFO(LOG_TAG, "feedback path is already configured");
            goto start_pcm;
        }

        codecTagId = (codecType == DEC ? BT_PLACEHOLDER_ENCODER : BT_PLACEHOLDER_DECODER);
        ret = SessionAlsaUtils::getModuleInstanceId(mixerHandle,
                     fbpcmDevIds.at(0), backEndName.c_str(), codecTagId, &miid);
        if (ret) {
            PAL_ERR(LOG_TAG, "getMiid for feedback device failed");
            goto err_pcm_open;
        }

        ret = getPluginPayload(&pluginLibHandle, &codec, &out_buf, (codecType == DEC ? ENC : DEC));
        if (ret) {
            PAL_ERR(LOG_TAG, "getPluginPayload failed");
            goto err_pcm_open;
        }

        /* SWB Encoder/Decoder has only 1 param, read block 0 */
        if (out_buf->num_blks != 1) {
            PAL_ERR(LOG_TAG, "incorrect block size %d", out_buf->num_blks);
            goto err_pcm_open;
        }
        fbDev->codecConfig.sample_rate = out_buf->sample_rate;
        fbDev->codecConfig.bit_width = out_buf->bit_format;
        fbDev->codecConfig.ch_info.channels = out_buf->channel_count;

        blk = out_buf->blocks[0];
        builder->payloadCustomParam(&paramData, &paramSize,
                  (uint32_t *)blk->payload, blk->payload_sz, miid, blk->param_id);

        codec->close_plugin(codec);
        dlclose(pluginLibHandle);

        if (!paramData) {
            PAL_ERR(LOG_TAG, "Failed to populateAPMHeader");
            ret = -ENOMEM;
            goto err_pcm_open;
        }

        ret = SessionAlsaUtils::setDeviceCustomPayload(rm, backEndName,
                paramData, paramSize);
        free(paramData);
        if (ret) {
            PAL_ERR(LOG_TAG, "Error: Dev setParam failed for %d", fbDevice.id);
            goto err_pcm_open;
        }
    } else if ((codecFormat == CODEC_TYPE_LC3) && (codecType == ENC)) {
        builder = new PayloadBuilder();

        if ((fbDevice.id == PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET) ||
            (fbDevice.id == PAL_DEVICE_OUT_BLUETOOTH_SCO)) {
            fbDev = std::dynamic_pointer_cast<BtSco>(BtSco::getInstance(&fbDevice, rm));
            if (!fbDev) {
                PAL_ERR(LOG_TAG, "failed to get BtSco singleton object.");
                goto err_pcm_open;
            }
        } else {
            fbDev = std::dynamic_pointer_cast<BtA2dp>(BtA2dp::getInstance(&fbDevice, rm));
            if (!fbDev) {
                PAL_ERR(LOG_TAG, "failed to get BtA2dp singleton object.");
                goto err_pcm_open;
            }
        }

        /* configure COP v2 depacketizer */
        ret = SessionAlsaUtils::getModuleInstanceId(mixerHandle,
                     fbpcmDevIds.at(0), backEndName.c_str(), COP_DEPACKETIZER_V2, &miid);
        if (ret) {
            PAL_ERR(LOG_TAG, "Failed to get tag info %x, ret = %d",
                    COP_DEPACKETIZER_V2, ret);
            goto err_pcm_open;
        }

        // intentionally configure depacketizer in the same manner as configuring packetizer
        builder->payloadCopV2PackConfig(&paramData, &paramSize, miid, codecInfo);
        if (!paramData) {
            PAL_ERR(LOG_TAG, "Invalid COPv2 module param size");
            ret = -EINVAL;
            goto err_pcm_open;
        }

        ret = SessionAlsaUtils::setDeviceCustomPayload(rm, backEndName,
                paramData, paramSize);
        delete [] paramData;
        if (ret) {
            PAL_ERR(LOG_TAG, "Error: Dev setParam failed for %d", fbDevice.id);
            goto err_pcm_open;
        }
    }

start_pcm:
    config.rate = SAMPLINGRATE_8K;
    config.format = PCM_FORMAT_S16_LE;
    config.channels = CHANNELS_1;
    config.period_size = 240;
    config.period_count = 2;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;
    fbPcm = pcm_open(rm->getSndCard(), fbpcmDevIds.at(0), flags, &config);
    if (!fbPcm) {
        PAL_ERR(LOG_TAG, "pcm open failed");
        goto free_fe;
    }

    if (!pcm_is_ready(fbPcm)) {
        PAL_ERR(LOG_TAG, "pcm open not ready");
        goto err_pcm_open;
    }

    ret = pcm_start(fbPcm);
    if (ret) {
        PAL_ERR(LOG_TAG, "pcm_start rx failed %d", ret);
        goto err_pcm_open;
    }

    if (codecFormat == CODEC_TYPE_APTX_AD_SPEECH) {
        fbDev->isConfigured = true;
        fbDev->deviceCount++;
    }
    if ((codecFormat == CODEC_TYPE_LC3) &&
        (fbDevice.id == PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET ||
         fbDevice.id == PAL_DEVICE_OUT_BLUETOOTH_SCO)) {
        fbDev->isConfigured = true;
        fbDev->deviceCount++;
    }

    abrRefCnt++;
    PAL_INFO(LOG_TAG, "Feedback Device started successfully");
    goto done;
err_pcm_open:
    pcm_close(fbPcm);
    fbPcm = NULL;
free_fe:
    rm->freeFrontEndIds(fbpcmDevIds, sAttr, dir);
    fbpcmDevIds.clear();
done:
    mAbrMutex.unlock();
    if (builder) {
       delete builder;
       builder = NULL;
    }
    return;
}

void Bluetooth::stopAbr()
{
    struct pal_stream_attributes sAttr;
    struct mixer_ctl *btSetFeedbackChannelCtrl = NULL;
    struct mixer *mixerHandle = NULL;
    int dir, ret = 0;

    mAbrMutex.lock();
    if (!fbPcm) {
        PAL_ERR(LOG_TAG, "fbPcm is null");
        mAbrMutex.unlock();
        return;
    }

    if (--abrRefCnt != 0) {
        PAL_DBG(LOG_TAG, "abrRefCnt is %d", abrRefCnt);
        mAbrMutex.unlock();
        return;
    }

    memset(&sAttr, 0, sizeof(sAttr));
    sAttr.type = PAL_STREAM_LOW_LATENCY;
    sAttr.direction = PAL_AUDIO_INPUT_OUTPUT;

    pcm_stop(fbPcm);
    pcm_close(fbPcm);

    ret = rm->getAudioMixer(&mixerHandle);
    if (ret) {
        PAL_ERR(LOG_TAG, "get mixer handle failed %d", ret);
        goto free_fe;
    }
    // Reset BT driver mixer control for ABR usecase
    btSetFeedbackChannelCtrl = mixer_get_ctl_by_name(mixerHandle,
                                        MIXER_SET_FEEDBACK_CHANNEL);
    if (!btSetFeedbackChannelCtrl) {
        PAL_ERR(LOG_TAG, "%s mixer control not identified",
                MIXER_SET_FEEDBACK_CHANNEL);
    } else if (mixer_ctl_set_value(btSetFeedbackChannelCtrl, 0, 0) != 0) {
        PAL_ERR(LOG_TAG, "Failed to reset BT usecase");
    }

    if ((codecFormat == CODEC_TYPE_APTX_AD_SPEECH) && fbDev) {
        if (--fbDev->deviceCount == 0) {
            fbDev->isConfigured = false;
        }
    }
    if ((codecFormat == CODEC_TYPE_LC3) &&
        (deviceAttr.id == PAL_DEVICE_OUT_BLUETOOTH_SCO ||
         deviceAttr.id == PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET) &&
        fbDev) {
        if (--fbDev->deviceCount == 0) {
            fbDev->isConfigured = false;
        }
    }

free_fe:
    dir = ((codecType == DEC) ? RX_HOSTLESS : TX_HOSTLESS);
    if (fbpcmDevIds.size()) {
        rm->freeFrontEndIds(fbpcmDevIds, sAttr, dir);
        fbpcmDevIds.clear();
    }
    isAbrEnabled = false;
    mAbrMutex.unlock();
}


/* Scope of BtA2dp class */
// definition of static BtA2dp member variables
std::shared_ptr<Device> BtA2dp::objRx = nullptr;
std::shared_ptr<Device> BtA2dp::objTx = nullptr;
void *BtA2dp::bt_lib_source_handle = nullptr;
void *BtA2dp::bt_lib_sink_handle = nullptr;
bt_audio_pre_init_t BtA2dp::bt_audio_pre_init = nullptr;
audio_source_open_t BtA2dp::audio_source_open = nullptr;
audio_source_close_t BtA2dp::audio_source_close = nullptr;
audio_source_start_t BtA2dp::audio_source_start = nullptr;
audio_source_stop_t BtA2dp::audio_source_stop = nullptr;
audio_source_suspend_t BtA2dp::audio_source_suspend = nullptr;
audio_source_handoff_triggered_t BtA2dp::audio_source_handoff_triggered = nullptr;
clear_source_a2dpsuspend_flag_t BtA2dp::clear_source_a2dpsuspend_flag = nullptr;
audio_get_enc_config_t BtA2dp::audio_get_enc_config = nullptr;
audio_source_check_a2dp_ready_t BtA2dp::audio_source_check_a2dp_ready = nullptr;
audio_is_tws_mono_mode_enable_t BtA2dp::audio_is_tws_mono_mode_enable = nullptr;
audio_sink_get_a2dp_latency_t BtA2dp::audio_sink_get_a2dp_latency = nullptr;
audio_sink_start_t BtA2dp::audio_sink_start = nullptr;
audio_sink_stop_t BtA2dp::audio_sink_stop = nullptr;
audio_get_dec_config_t BtA2dp::audio_get_dec_config = nullptr;
audio_sink_session_setup_complete_t BtA2dp::audio_sink_session_setup_complete = nullptr;
audio_sink_check_a2dp_ready_t BtA2dp::audio_sink_check_a2dp_ready = nullptr;


BtA2dp::BtA2dp(struct pal_device *device, std::shared_ptr<ResourceManager> Rm)
      : Bluetooth(device, Rm),
        a2dpState(A2DP_STATE_DISCONNECTED),
        totalActiveSessionRequests(0)
{
    a2dpRole = (device->id == PAL_DEVICE_IN_BLUETOOTH_A2DP) ? SINK : SOURCE;
    codecType = (device->id == PAL_DEVICE_IN_BLUETOOTH_A2DP) ? DEC : ENC;
    pluginHandler = NULL;
    pluginCodec = NULL;

    init();
    param_bt_a2dp.reconfig = false;
    param_bt_a2dp.a2dp_suspended = false;
    param_bt_a2dp.is_force_switch = false;
    isA2dpOffloadSupported =
            property_get_bool("ro.bluetooth.a2dp_offload.supported", false) &&
            !property_get_bool("persist.bluetooth.a2dp_offload.disabled", false);

    PAL_DBG(LOG_TAG, "A2DP offload supported = %d",
            isA2dpOffloadSupported);
    param_bt_a2dp.reconfig_supported = isA2dpOffloadSupported;
    param_bt_a2dp.latency = 0;
}

BtA2dp::~BtA2dp()
{
}

void BtA2dp::open_a2dp_source()
{
    int ret = 0;

    PAL_DBG(LOG_TAG, "Open A2DP source start");
    if (bt_lib_source_handle && audio_source_open) {
        if (a2dpState == A2DP_STATE_DISCONNECTED) {
            PAL_DBG(LOG_TAG, "calling BT stream open");
            ret = audio_source_open();
            if (ret != 0) {
                PAL_ERR(LOG_TAG, "Failed to open source stream for a2dp: status %d", ret);
            }
            a2dpState = A2DP_STATE_CONNECTED;
        } else {
            PAL_DBG(LOG_TAG, "Called a2dp open with improper state %d", a2dpState);
        }
    }
}

int BtA2dp::close_audio_source()
{
    PAL_VERBOSE(LOG_TAG, "Enter");

    if (!(bt_lib_source_handle && audio_source_close)) {
        PAL_ERR(LOG_TAG, "a2dp source handle is not identified, Ignoring close request");
        return -ENOSYS;
    }

    if (a2dpState != A2DP_STATE_DISCONNECTED) {
        PAL_DBG(LOG_TAG, "calling BT source stream close");
        if (audio_source_close() == false)
            PAL_ERR(LOG_TAG, "failed close a2dp source control path from BT library");
    }
    totalActiveSessionRequests = 0;
    param_bt_a2dp.a2dp_suspended = false;
    param_bt_a2dp.reconfig = false;
    param_bt_a2dp.latency = 0;
    a2dpState = A2DP_STATE_DISCONNECTED;

    return 0;
}

void BtA2dp::init_a2dp_source()
{
    PAL_DBG(LOG_TAG, "init_a2dp_source START");
    if (bt_lib_source_handle == nullptr) {
        PAL_DBG(LOG_TAG, "Requesting for BT lib handle");
        bt_lib_source_handle = dlopen(BT_IPC_SOURCE_LIB, RTLD_NOW);
        if (bt_lib_source_handle == nullptr) {
            PAL_ERR(LOG_TAG, "dlopen failed for %s", BT_IPC_SOURCE_LIB);
            return;
        }
    }
    bt_audio_pre_init = (bt_audio_pre_init_t)
                  dlsym(bt_lib_source_handle, "bt_audio_pre_init");
    audio_source_open = (audio_source_open_t)
                  dlsym(bt_lib_source_handle, "audio_stream_open");
    audio_source_start = (audio_source_start_t)
                  dlsym(bt_lib_source_handle, "audio_start_stream");
    audio_get_enc_config = (audio_get_enc_config_t)
                  dlsym(bt_lib_source_handle, "audio_get_codec_config");
    audio_source_suspend = (audio_source_suspend_t)
                  dlsym(bt_lib_source_handle, "audio_suspend_stream");
    audio_source_handoff_triggered = (audio_source_handoff_triggered_t)
                  dlsym(bt_lib_source_handle, "audio_handoff_triggered");
    clear_source_a2dpsuspend_flag = (clear_source_a2dpsuspend_flag_t)
                  dlsym(bt_lib_source_handle, "clear_a2dpsuspend_flag");
    audio_source_stop = (audio_source_stop_t)
                  dlsym(bt_lib_source_handle, "audio_stop_stream");
    audio_source_close = (audio_source_close_t)
                  dlsym(bt_lib_source_handle, "audio_stream_close");
    audio_source_check_a2dp_ready = (audio_source_check_a2dp_ready_t)
                  dlsym(bt_lib_source_handle, "audio_check_a2dp_ready");
    audio_sink_get_a2dp_latency = (audio_sink_get_a2dp_latency_t)
                  dlsym(bt_lib_source_handle, "audio_sink_get_a2dp_latency");
    audio_is_tws_mono_mode_enable = (audio_is_tws_mono_mode_enable_t)
                  dlsym(bt_lib_source_handle, "isTwsMonomodeEnable");

    if (bt_lib_source_handle && bt_audio_pre_init) {
        PAL_DBG(LOG_TAG, "calling BT module preinit");
        bt_audio_pre_init();
    }
    usleep(20 * 1000); //TODO: to add interval properly
    open_a2dp_source();
}

void BtA2dp::init_a2dp_sink()
{
    PAL_DBG(LOG_TAG, "Open A2DP input start");
    if (bt_lib_sink_handle == nullptr) {
        PAL_DBG(LOG_TAG, "Requesting for BT lib handle");
        bt_lib_sink_handle = dlopen(BT_IPC_SINK_LIB, RTLD_NOW);

        if (bt_lib_sink_handle == nullptr) {
#ifndef LINUX_ENABLED
            // On Mobile LE VoiceBackChannel implemented as A2DPSink Profile.
            // However - All the BT-Host IPC calls are exposed via Source LIB itself.
            PAL_DBG(LOG_TAG, "Requesting for BT lib source handle");
            bt_lib_sink_handle = dlopen(BT_IPC_SOURCE_LIB, RTLD_NOW);
            if (bt_lib_sink_handle == nullptr) {
                PAL_ERR(LOG_TAG, "DLOPEN failed");
                return;
            }
            isDummySink = true;
            audio_get_enc_config = (audio_get_enc_config_t)
                  dlsym(bt_lib_sink_handle, "audio_get_codec_config");
            audio_sink_get_a2dp_latency = (audio_sink_get_a2dp_latency_t)
                dlsym(bt_lib_sink_handle, "audio_sink_get_a2dp_latency");
            audio_source_start = (audio_source_start_t)
                  dlsym(bt_lib_sink_handle, "audio_start_stream");
            audio_source_stop = (audio_source_stop_t)
                  dlsym(bt_lib_sink_handle, "audio_stop_stream");
            audio_source_check_a2dp_ready = (audio_source_check_a2dp_ready_t)
                  dlsym(bt_lib_sink_handle, "audio_check_a2dp_ready");
#else
            // On Linux Builds - A2DP Sink Profile is supported via different lib
            PAL_ERR(LOG_TAG, "DLOPEN failed for %s", BT_IPC_SINK_LIB);
#endif
        } else {
            audio_sink_start = (audio_sink_start_t)
                          dlsym(bt_lib_sink_handle, "audio_sink_start_capture");
            audio_get_dec_config = (audio_get_dec_config_t)
                          dlsym(bt_lib_sink_handle, "audio_get_decoder_config");
            audio_sink_stop = (audio_sink_stop_t)
                          dlsym(bt_lib_sink_handle, "audio_sink_stop_capture");
            audio_sink_check_a2dp_ready = (audio_sink_check_a2dp_ready_t)
                          dlsym(bt_lib_sink_handle, "audio_sink_check_a2dp_ready");
            audio_sink_session_setup_complete = (audio_sink_session_setup_complete_t)
                          dlsym(bt_lib_sink_handle, "audio_sink_session_setup_complete");
        }
    }
}

bool BtA2dp::a2dp_send_sink_setup_complete()
{
    uint64_t system_latency = 0;
    bool is_complete = false;

    /* TODO : Replace this with call to plugin */
    system_latency = 200;

    if (audio_sink_session_setup_complete(system_latency) == 0) {
        is_complete = true;
    }
    return is_complete;
}

void BtA2dp::init()
{
    (a2dpRole == SOURCE) ? init_a2dp_source() : init_a2dp_sink();
}

int BtA2dp::start()
{
    int status = 0;
    mDeviceMutex.lock();

    if (customPayload)
        free(customPayload);

    customPayload = NULL;
    customPayloadSize = 0;

    status = (a2dpRole == SOURCE) ? startPlayback() : startCapture();
    if (status != 0) {
        mDeviceMutex.unlock();
        return status;
    }

    status = Device::start_l();

    if (!status && isAbrEnabled)
        startAbr();

    mDeviceMutex.unlock();
    return status;
}

int BtA2dp::stop()
{
    int status = 0;
    mDeviceMutex.lock();

    if (isAbrEnabled)
        stopAbr();

    Device::stop_l();

    status = (a2dpRole == SOURCE) ? stopPlayback() : stopCapture();
    mDeviceMutex.unlock();

    return status;
}

int BtA2dp::startPlayback()
{
    int ret = 0;
    uint8_t multi_cast = 0, num_dev = 1;

    PAL_DBG(LOG_TAG, "a2dp_start_playback start");

    if (!(bt_lib_source_handle && audio_source_start
                && audio_get_enc_config)) {
        PAL_ERR(LOG_TAG, "a2dp handle is not identified, Ignoring start playback request");
        return -ENOSYS;
    }

    if (param_bt_a2dp.a2dp_suspended) {
        // session will be restarted after suspend completion
        PAL_INFO(LOG_TAG, "a2dp start requested during suspend state");
        return 0;
    }

    if (a2dpState != A2DP_STATE_STARTED && !totalActiveSessionRequests) {
        codecFormat = CODEC_TYPE_INVALID;
        PAL_DBG(LOG_TAG, "calling BT module stream start");
        /* This call indicates BT IPC lib to start playback */
        ret =  audio_source_start();
        PAL_ERR(LOG_TAG, "BT controller start return = %d",ret);
        if (ret != 0) {
           PAL_ERR(LOG_TAG, "BT controller start failed");
           return ret;
        }

        PAL_DBG(LOG_TAG, "configure_a2dp_encoder_format start");
        codecInfo = audio_get_enc_config(&multi_cast, &num_dev, (audio_format_t *)&codecFormat);
        if (codecInfo == NULL || codecFormat == CODEC_TYPE_INVALID) {
            PAL_ERR(LOG_TAG, "invalid encoder config");
            audio_source_stop();
            return -EINVAL;
        }

        if (codecFormat == CODEC_TYPE_APTX_DUAL_MONO && audio_is_tws_mono_mode_enable)
            isTwsMonoModeOn = audio_is_tws_mono_mode_enable();

        /* Update Device GKV based on Encoder type */
        updateDeviceMetadata();
        ret = configureA2dpEncoderDecoder();
        if (ret) {
            PAL_ERR(LOG_TAG, "unable to configure DSP encoder");
            audio_source_stop();
            return ret;
        }

        /* Reset device GKV for AAC ABR */
        if ((codecFormat == CODEC_TYPE_AAC) && isAbrEnabled)
            updateDeviceMetadata();

        a2dpState = A2DP_STATE_STARTED;
    } else {
        /* Update Device GKV based on Already received encoder. */
        /* This is required for getting tagged module info in session class. */
        updateDeviceMetadata();
    }

    totalActiveSessionRequests++;
    PAL_DBG(LOG_TAG, "start A2DP playback total active sessions :%d",
            totalActiveSessionRequests);
    return ret;
}

int BtA2dp::stopPlayback()
{
    int ret =0;

    PAL_VERBOSE(LOG_TAG, "a2dp_stop_playback start");
    if (!(bt_lib_source_handle && audio_source_stop)) {
        PAL_ERR(LOG_TAG, "a2dp handle is not identified, Ignoring stop request");
        return -ENOSYS;
    }

    if (totalActiveSessionRequests > 0)
        totalActiveSessionRequests--;
    else
        PAL_ERR(LOG_TAG, "No active playback session requests on A2DP");

    if (a2dpState == A2DP_STATE_STARTED && !totalActiveSessionRequests) {
        PAL_VERBOSE(LOG_TAG, "calling BT module stream stop");
        ret = audio_source_stop();
        if (ret < 0) {
            PAL_ERR(LOG_TAG, "stop stream to BT IPC lib failed");
        } else {
            PAL_VERBOSE(LOG_TAG, "stop steam to BT IPC lib successful");
        }
        isConfigured = false;
        a2dpState = A2DP_STATE_STOPPED;
        codecInfo = NULL;

        /* Reset isTwsMonoModeOn and isLC3MonoModeOn during stop */
        if (!param_bt_a2dp.a2dp_suspended) {
            isTwsMonoModeOn = false;
            isLC3MonoModeOn = false;
        }

        if (pluginCodec) {
            pluginCodec->close_plugin(pluginCodec);
            pluginCodec = NULL;
        }
        if (pluginHandler) {
            dlclose(pluginHandler);
            pluginHandler = NULL;
        }
    }

    PAL_DBG(LOG_TAG, "Stop A2DP playback, total active sessions :%d",
            totalActiveSessionRequests);
    return 0;
}

bool BtA2dp::isDeviceReady()
{
    bool ret = false;

    if (param_bt_a2dp.a2dp_suspended)
        return ret;

    if ((a2dpState != A2DP_STATE_DISCONNECTED) &&
        (isA2dpOffloadSupported)) {
        if ((a2dpRole == SOURCE) || isDummySink) {
            if (audio_source_check_a2dp_ready)
                ret = audio_source_check_a2dp_ready();
        } else {
            if (audio_sink_check_a2dp_ready)
                ret = audio_sink_check_a2dp_ready();
        }
    }
    return ret;
}

int BtA2dp::startCapture()
{
    int ret = 0;

    PAL_DBG(LOG_TAG, "a2dp_start_capture start");

    codecFormat = CODEC_TYPE_INVALID;

    if (!isDummySink) {
        if (!(bt_lib_sink_handle && audio_sink_start
            && audio_get_dec_config)) {
            PAL_ERR(LOG_TAG, "a2dp handle is not identified, Ignoring start capture request");
            return -ENOSYS;
        }

        if (a2dpState != A2DP_STATE_STARTED  && !totalActiveSessionRequests) {
            PAL_DBG(LOG_TAG, "calling BT module stream start");
            /* This call indicates BT IPC lib to start capture */
            ret =  audio_sink_start();
            PAL_ERR(LOG_TAG, "BT controller start capture return = %d",ret);
            if (ret != 0 ) {
                PAL_ERR(LOG_TAG, "BT controller start capture failed");
                return ret;
            }

            codecInfo = audio_get_dec_config((audio_format_t *)&codecFormat);
            if (codecInfo == NULL || codecFormat == CODEC_TYPE_INVALID) {
                PAL_ERR(LOG_TAG, "invalid encoder config");
                return -EINVAL;
            }
        }
    } else {
        uint8_t multi_cast = 0, num_dev = 1;

        if (!(bt_lib_sink_handle && audio_source_start
            && audio_get_enc_config)) {
            PAL_ERR(LOG_TAG, "a2dp handle is not identified, Ignoring start capture request");
            return -ENOSYS;
        }

        if (a2dpState != A2DP_STATE_STARTED  && !totalActiveSessionRequests) {
            PAL_DBG(LOG_TAG, "calling BT module stream start");
            /* This call indicates BT IPC lib to start */
            ret =  audio_source_start();
            PAL_ERR(LOG_TAG, "BT controller start return = %d",ret);
            if (ret != 0 ) {
                PAL_ERR(LOG_TAG, "BT controller start failed");
                return ret;
            }

            codecInfo = audio_get_enc_config(&multi_cast, &num_dev, (audio_format_t *)&codecFormat);
            if (codecInfo == NULL || codecFormat == CODEC_TYPE_INVALID) {
                PAL_ERR(LOG_TAG, "invalid codec config");
                return -EINVAL;
            }
        }
    }
    /* Update Device GKV based on Decoder type */
    updateDeviceMetadata();

    ret = configureA2dpEncoderDecoder();
    if (ret) {
        PAL_DBG(LOG_TAG, "unable to configure DSP decoder");
        return ret;
    }

    if (!isDummySink) {
        if (!a2dp_send_sink_setup_complete()) {
            PAL_DBG(LOG_TAG, "sink_setup_complete not successful");
            ret = -ETIMEDOUT;
        }
    }
    if (a2dpState != A2DP_STATE_STARTED  && !totalActiveSessionRequests) {
        totalActiveSessionRequests++;
        a2dpState = A2DP_STATE_STARTED;
    }

    PAL_DBG(LOG_TAG, "start A2DP sink total active sessions :%d",
                      totalActiveSessionRequests);
    return ret;
}

int BtA2dp::stopCapture()
{
    int ret =0;

    PAL_VERBOSE(LOG_TAG, "a2dp_stop_capture start");
    if (!isDummySink && !(bt_lib_sink_handle && audio_sink_stop)) {
        PAL_ERR(LOG_TAG, "a2dp handle is not identified, Ignoring stop request");
        return -ENOSYS;
    }

    if (totalActiveSessionRequests > 0)
        totalActiveSessionRequests--;

    if (a2dpState == A2DP_STATE_STARTED && !totalActiveSessionRequests) {
        PAL_VERBOSE(LOG_TAG, "calling BT module stream stop");
        isConfigured = false;
        if (!isDummySink) {
            ret = audio_sink_stop();
            if (ret < 0) {
                PAL_ERR(LOG_TAG, "stop stream to BT IPC lib failed");
            } else {
                PAL_VERBOSE(LOG_TAG, "stop steam to BT IPC lib successful");
            }
        } else {
            ret = audio_source_stop();
            if (ret < 0) {
                PAL_ERR(LOG_TAG, "stop stream to BT IPC lib failed");
            } else {
                PAL_VERBOSE(LOG_TAG, "stop steam to BT IPC lib successful");
            }
        }
        a2dpState = A2DP_STATE_STOPPED;

        if (pluginCodec) {
            pluginCodec->close_plugin(pluginCodec);
            pluginCodec = NULL;
        }
        if (pluginHandler) {
            dlclose(pluginHandler);
            pluginHandler = NULL;
        }
    }
    PAL_DBG(LOG_TAG, "Stop A2DP capture, total active sessions :%d",
            totalActiveSessionRequests);
    return 0;
}

int32_t BtA2dp::setDeviceParameter(uint32_t param_id, void *param)
{
    int32_t status = 0;
    pal_param_bta2dp_t* param_a2dp = (pal_param_bta2dp_t *)param;

    if (isA2dpOffloadSupported == false) {
       PAL_VERBOSE(LOG_TAG, "no supported encoders identified,ignoring a2dp setparam");
       status = -EINVAL;
       goto exit;
    }

    switch(param_id) {
    case PAL_PARAM_ID_DEVICE_CONNECTION:
    {
        pal_param_device_connection_t *device_connection =
            (pal_param_device_connection_t *)param;
        if (device_connection->connection_state == true) {
            if (a2dpRole == SOURCE)
                open_a2dp_source();
            else {
                a2dpState = A2DP_STATE_CONNECTED;
            }
        } else {
            if (a2dpRole == SOURCE) {
                status = close_audio_source();
            } else {
                totalActiveSessionRequests = 0;
                param_bt_a2dp.a2dp_suspended = false;
                param_bt_a2dp.reconfig = false;
                param_bt_a2dp.latency = 0;
                a2dpState = A2DP_STATE_DISCONNECTED;
            }
        }
        break;
    }
    case PAL_PARAM_ID_BT_A2DP_RECONFIG:
    {
        if (a2dpState != A2DP_STATE_DISCONNECTED) {
            param_bt_a2dp.reconfig = param_a2dp->reconfig;
        }
        break;
    }
    case PAL_PARAM_ID_BT_A2DP_SUSPENDED:
    {
        if (bt_lib_source_handle == nullptr)
            goto exit;

        if (param_bt_a2dp.a2dp_suspended == param_a2dp->a2dp_suspended)
            goto exit;

        if (param_a2dp->a2dp_suspended == true) {
            param_bt_a2dp.a2dp_suspended = true;
            if (a2dpState == A2DP_STATE_DISCONNECTED)
                goto exit;

            rm->a2dpSuspend();
            if (audio_source_suspend)
                audio_source_suspend();
        } else {
            if (clear_source_a2dpsuspend_flag)
                clear_source_a2dpsuspend_flag();

            param_bt_a2dp.a2dp_suspended = false;

            if (totalActiveSessionRequests > 0) {
                if (audio_source_start) {
                    status = audio_source_start();
                    if (status) {
                        PAL_ERR(LOG_TAG, "BT controller start failed");
                        goto exit;
                    }
                }
            }
            rm->a2dpResume();
        }
        break;
    }
    case PAL_PARAM_ID_BT_A2DP_TWS_CONFIG:
    {
        isTwsMonoModeOn = param_a2dp->is_tws_mono_mode_on;
        if (a2dpState == A2DP_STATE_STARTED) {
            std::shared_ptr<Device> dev = nullptr;
            Stream *stream = NULL;
            Session *session = NULL;
            std::vector<Stream*> activestreams;
            pal_bt_tws_payload param_tws;

            dev = Device::getInstance(&deviceAttr, rm);
            status = rm->getActiveStream_l(dev, activestreams);
            if ((0 != status) || (activestreams.size() == 0)) {
                PAL_ERR(LOG_TAG, "no active stream available");
                return -EINVAL;
            }
            stream = static_cast<Stream *>(activestreams[0]);
            stream->getAssociatedSession(&session);
            param_tws.isTwsMonoModeOn = isTwsMonoModeOn;
            param_tws.codecFormat = (uint32_t)codecFormat;
            session->setParameters(stream, BT_PLACEHOLDER_ENCODER, param_id, &param_tws);
        }
        break;
    }
    case PAL_PARAM_ID_BT_A2DP_LC3_CONFIG:
    {
        isLC3MonoModeOn = param_a2dp->is_lc3_mono_mode_on;
        if (a2dpState == A2DP_STATE_STARTED) {
            std::shared_ptr<Device> dev = nullptr;
            Stream *stream = NULL;
            Session *session = NULL;
            std::vector<Stream*> activestreams;
            pal_bt_lc3_payload param_lc3;

            dev = Device::getInstance(&deviceAttr, rm);
            status = rm->getActiveStream_l(dev, activestreams);
            if ((0 != status) || (activestreams.size() == 0)) {
                PAL_ERR(LOG_TAG, "no active stream available");
                return -EINVAL;
            }
            stream = static_cast<Stream *>(activestreams[0]);
            stream->getAssociatedSession(&session);
            param_lc3.isLC3MonoModeOn = isLC3MonoModeOn;
            session->setParameters(stream, BT_PLACEHOLDER_ENCODER, param_id, &param_lc3);
        }
        break;
    }
    default:
        return -EINVAL;
    }

exit:
    return status;
}

int32_t BtA2dp::getDeviceParameter(uint32_t param_id, void **param)
{
    switch (param_id) {
    case PAL_PARAM_ID_BT_A2DP_RECONFIG:
    case PAL_PARAM_ID_BT_A2DP_RECONFIG_SUPPORTED:
    case PAL_PARAM_ID_BT_A2DP_SUSPENDED:
        *param = &param_bt_a2dp;
        break;
    case PAL_PARAM_ID_BT_A2DP_DECODER_LATENCY:
    case PAL_PARAM_ID_BT_A2DP_ENCODER_LATENCY:
    {
        uint32_t slatency = 0;
        if (audio_sink_get_a2dp_latency && (a2dpState != A2DP_STATE_DISCONNECTED))
            slatency = audio_sink_get_a2dp_latency();
        if (pluginCodec) {
            param_bt_a2dp.latency =
                pluginCodec->plugin_get_codec_latency(pluginCodec, slatency);
        } else {
            param_bt_a2dp.latency = 0;
        }
        *param = &param_bt_a2dp;
        break;
    }
    case PAL_PARAM_ID_BT_A2DP_FORCE_SWITCH:
    {
        if (param_bt_a2dp.reconfig || (a2dpState != A2DP_STATE_STARTED)) {
            param_bt_a2dp.is_force_switch = true;
        } else {
            param_bt_a2dp.is_force_switch = false;
        }

        *param = &param_bt_a2dp;
        break;
    }
    default:
        return -EINVAL;
    }
    return 0;
}

std::shared_ptr<Device> BtA2dp::getObject(pal_device_id_t id)
{
    if (id == PAL_DEVICE_OUT_BLUETOOTH_A2DP)
        return objRx;
    else
        return objTx;
}

std::shared_ptr<Device>
BtA2dp::getInstance(struct pal_device *device, std::shared_ptr<ResourceManager> Rm)
{
    if (device->id == PAL_DEVICE_OUT_BLUETOOTH_A2DP) {
        if (!objRx) {
            PAL_INFO(LOG_TAG, "creating instance for  %d", device->id);
            std::shared_ptr<Device> sp(new BtA2dp(device, Rm));
            objRx = sp;
        }
        return objRx;
    } else {
        if (!objTx) {
            PAL_INFO(LOG_TAG, "creating instance for  %d", device->id);
            std::shared_ptr<Device> sp(new BtA2dp(device, Rm));
            objTx = sp;
        }
        return objTx;
    }
}

/* Scope of BtScoRX/Tx class */
std::shared_ptr<Device> BtSco::objRx = nullptr;
std::shared_ptr<Device> BtSco::objTx = nullptr;

BtSco::BtSco(struct pal_device *device, std::shared_ptr<ResourceManager> Rm)
    : Bluetooth(device, Rm),
      isScoOn(false),
      isWbSpeechEnabled(false),
      swbSpeechMode(SPEECH_MODE_INVALID),
      isSwbLc3Enabled(false)
{
    codecType = (device->id == PAL_DEVICE_OUT_BLUETOOTH_SCO) ? ENC : DEC;
}

BtSco::~BtSco()
{
    if (lc3CodecInfo.enc_cfg.streamMapOut != NULL)
        delete [] lc3CodecInfo.enc_cfg.streamMapOut;
    if (lc3CodecInfo.dec_cfg.streamMapIn != NULL)
        delete [] lc3CodecInfo.dec_cfg.streamMapIn;
}

bool BtSco::isDeviceReady()
{
    return isScoOn;
}

void BtSco::updateSampleRate(uint32_t *sampleRate)
{
    if (isWbSpeechEnabled)
        *sampleRate = SAMPLINGRATE_16K;
    else if (swbSpeechMode != SPEECH_MODE_INVALID)
        *sampleRate = SAMPLINGRATE_96K;
    else if (isSwbLc3Enabled)
        *sampleRate = SAMPLINGRATE_96K;
    else
        *sampleRate = deviceAttr.config.sample_rate;
}

int32_t BtSco::setDeviceParameter(uint32_t param_id, void *param)
{
    pal_param_btsco_t* param_bt_sco = (pal_param_btsco_t *)param;

    switch (param_id) {
    case PAL_PARAM_ID_BT_SCO:
        isScoOn = param_bt_sco->bt_sco_on;
        break;
    case PAL_PARAM_ID_BT_SCO_WB:
        isWbSpeechEnabled = param_bt_sco->bt_wb_speech_enabled;
        PAL_DBG(LOG_TAG, "isWbSpeechEnabled = %d", isWbSpeechEnabled);
        break;
    case PAL_PARAM_ID_BT_SCO_SWB:
        swbSpeechMode = param_bt_sco->bt_swb_speech_mode;
        codecInfo = (void *)&swbSpeechMode;
        PAL_DBG(LOG_TAG, "swbSpeechMode = %d", swbSpeechMode);
        break;
    case PAL_PARAM_ID_BT_SCO_LC3:
        isSwbLc3Enabled = param_bt_sco->bt_lc3_speech_enabled;
        // parse sco lc3 parameters and pack into codec info
        convertCodecInfo(lc3CodecInfo, param_bt_sco->lc3_cfg);
        codecInfo = (void *)&lc3CodecInfo;
        PAL_DBG(LOG_TAG, "isSwbLc3Enabled = %d", isSwbLc3Enabled);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

void BtSco::convertCodecInfo(audio_lc3_codec_cfg_t &lc3CodecInfo,
                             btsco_lc3_cfg_t &lc3Cfg)
{
    std::vector<lc3_stream_map_t> steamMap;
    uint32_t audio_location = 0;
    uint8_t stream_id;
    uint8_t direction;
    int idx = 0;

    // convert and fill in encoder cfg
    lc3CodecInfo.enc_cfg.toAirConfig.sampling_freq        = LC3_CSC[lc3Cfg.rxconfig_index].sampling_freq;
    lc3CodecInfo.enc_cfg.toAirConfig.max_octets_per_frame = LC3_CSC[lc3Cfg.rxconfig_index].max_octets_per_frame;
    lc3CodecInfo.enc_cfg.toAirConfig.frame_duration       = LC3_CSC[lc3Cfg.rxconfig_index].frame_duration;
    lc3CodecInfo.enc_cfg.toAirConfig.bit_depth            = LC3_CSC[lc3Cfg.rxconfig_index].bit_depth;
    if (lc3Cfg.fields_map & LC3_FRAME_DURATION_BIT)
        lc3CodecInfo.enc_cfg.toAirConfig.frame_duration   = lc3Cfg.frame_duration;
    lc3CodecInfo.enc_cfg.toAirConfig.api_version          = lc3Cfg.api_version;
    lc3CodecInfo.enc_cfg.toAirConfig.num_blocks           = lc3Cfg.num_blocks;
    lc3CodecInfo.enc_cfg.toAirConfig.default_q_level      = 0;
    lc3CodecInfo.enc_cfg.toAirConfig.mode                 = 0x1;
    for (int i=0; i<16; i++)
        lc3CodecInfo.enc_cfg.toAirConfig.vendor_specific[i] = 0;

    // convert and fill in decoder cfg
    lc3CodecInfo.dec_cfg.fromAirConfig.sampling_freq        = LC3_CSC[lc3Cfg.txconfig_index].sampling_freq;
    lc3CodecInfo.dec_cfg.fromAirConfig.max_octets_per_frame = LC3_CSC[lc3Cfg.txconfig_index].max_octets_per_frame;
    lc3CodecInfo.dec_cfg.fromAirConfig.frame_duration       = LC3_CSC[lc3Cfg.txconfig_index].frame_duration;
    lc3CodecInfo.dec_cfg.fromAirConfig.bit_depth            = LC3_CSC[lc3Cfg.txconfig_index].bit_depth;
    if (lc3Cfg.fields_map & LC3_FRAME_DURATION_BIT)
        lc3CodecInfo.dec_cfg.fromAirConfig.frame_duration   = lc3Cfg.frame_duration;
    lc3CodecInfo.dec_cfg.fromAirConfig.api_version          = lc3Cfg.api_version;
    lc3CodecInfo.dec_cfg.fromAirConfig.num_blocks           = lc3Cfg.num_blocks;
    lc3CodecInfo.dec_cfg.fromAirConfig.default_q_level      = 0;
    lc3CodecInfo.dec_cfg.fromAirConfig.mode                 = 0x1;
    for (int i=0; i<16; i++)
        lc3CodecInfo.dec_cfg.fromAirConfig.vendor_specific[i] = 0;

    // parse stream map string and append stream map structures
    std::string s(lc3Cfg.streamMap);
    std::regex pattern("([0-9])[,[:s:]]+([0-9])[,[:s:]]+([MLR])");
    std::smatch match;
    while (std::regex_search(s, match, pattern)) {
        stream_id = atoi(match[1].str().c_str());
        direction = atoi(match[2].str().c_str());
        if (!strcmp(match[3].str().c_str(), "M")) {
            audio_location = 0;
        } else if (!strcmp(match[3].str().c_str(), "L")) {
            audio_location = 1;
        } else if (!strcmp(match[3].str().c_str(), "R")) {
            audio_location = 2;
        }

        if ((stream_id > 1) || (direction > 1) || (audio_location > 2)) {
            PAL_ERR(LOG_TAG, "invalid stream info (%d, %d, %d)", stream_id, direction, audio_location);
            continue;
        }
        steamMap.push_back({audio_location, stream_id, direction});
        s = match.suffix().str();
    }

    if (steamMap.size() == 0) {
        PAL_ERR(LOG_TAG, "invalid stream map size %d", steamMap.size());
        return;
    }

    lc3CodecInfo.enc_cfg.stream_map_size = steamMap.size();
    if (lc3CodecInfo.enc_cfg.streamMapOut != NULL)
        delete [] lc3CodecInfo.enc_cfg.streamMapOut;
    lc3CodecInfo.enc_cfg.streamMapOut = new lc3_stream_map_t[steamMap.size()];
    for (auto &it : steamMap) {
        lc3CodecInfo.enc_cfg.streamMapOut[idx++].audio_location = it.audio_location;
        lc3CodecInfo.enc_cfg.streamMapOut[idx++].stream_id = it.stream_id;
        lc3CodecInfo.enc_cfg.streamMapOut[idx++].direction = it.direction;
    }

    idx = 0;
    lc3CodecInfo.dec_cfg.stream_map_size = steamMap.size();
    if (lc3CodecInfo.dec_cfg.streamMapIn != NULL)
        delete [] lc3CodecInfo.dec_cfg.streamMapIn;
    lc3CodecInfo.dec_cfg.streamMapIn = new lc3_stream_map_t[steamMap.size()];
    for (auto &it : steamMap) {
        lc3CodecInfo.dec_cfg.streamMapIn[idx++].audio_location = it.audio_location;
        lc3CodecInfo.dec_cfg.streamMapIn[idx++].stream_id = it.stream_id;
        lc3CodecInfo.dec_cfg.streamMapIn[idx++].direction = it.direction;
    }

    if (lc3CodecInfo.dec_cfg.streamMapIn[0].audio_location == 0)
        lc3CodecInfo.dec_cfg.decoder_output_channel = CH_MONO;
    else
        lc3CodecInfo.dec_cfg.decoder_output_channel = CH_STEREO;
}

int BtSco::startSwb()
{
    int ret = 0;

    if (!isConfigured)
        ret = configureA2dpEncoderDecoder();

    return ret;
}

int BtSco::start()
{
    int status = 0;
    mDeviceMutex.lock();

    if (swbSpeechMode != SPEECH_MODE_INVALID) {
        codecFormat = CODEC_TYPE_APTX_AD_SPEECH;
    } else if (isSwbLc3Enabled) {
        codecFormat = CODEC_TYPE_LC3;
    }

    updateDeviceMetadata();
    if ((codecFormat == CODEC_TYPE_APTX_AD_SPEECH) ||
        (codecFormat == CODEC_TYPE_LC3)) {
        status = startSwb();
        if (status) {
            mDeviceMutex.unlock();
            return status;
        }
    } else {
        // For SCO NB and WB that don't have encoder and decoder in place,
        // just override codec configurations with device attributions.
        codecConfig.bit_width = deviceAttr.config.bit_width;
        codecConfig.sample_rate = deviceAttr.config.sample_rate;
        codecConfig.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
        codecConfig.ch_info.channels = deviceAttr.config.ch_info.channels;
        isConfigured = true;
        PAL_DBG(LOG_TAG, "SCO WB/NB codecConfig is same as deviceAttr bw = %d,sr = %d,ch = %d",
            codecConfig.bit_width, codecConfig.sample_rate, codecConfig.ch_info.channels);
    }

    status = Device::start_l();
    if (!status && isAbrEnabled)
        startAbr();

    mDeviceMutex.unlock();
    return status;
}

int BtSco::stop()
{
    int status = 0;
    mDeviceMutex.lock();

    if (isAbrEnabled)
        stopAbr();

    if (pluginCodec) {
        pluginCodec->close_plugin(pluginCodec);
        pluginCodec = NULL;
    }
    if (pluginHandler) {
        dlclose(pluginHandler);
        pluginHandler = NULL;
    }

    Device::stop_l();
    if (isAbrEnabled == false)
        codecFormat = CODEC_TYPE_INVALID;
    if (deviceCount == 0)
        isConfigured = false;

    mDeviceMutex.unlock();
    return status;
}

std::shared_ptr<Device> BtSco::getObject(pal_device_id_t id)
{
    if (id == PAL_DEVICE_OUT_BLUETOOTH_SCO)
        return objRx;
    else
        return objTx;
}

std::shared_ptr<Device> BtSco::getInstance(struct pal_device *device,
                                           std::shared_ptr<ResourceManager> Rm)
{
    if (device->id == PAL_DEVICE_OUT_BLUETOOTH_SCO) {
        if (!objRx) {
            std::shared_ptr<Device> sp(new BtSco(device, Rm));
            objRx = sp;
        }
        return objRx;
    } else {
        if (!objTx) {
            PAL_ERR(LOG_TAG,  "creating instance for  %d", device->id);
            std::shared_ptr<Device> sp(new BtSco(device, Rm));
            objTx = sp;
        }
        return objTx;
    }
}
/*BtSco class end */
