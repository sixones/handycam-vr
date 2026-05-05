/*
  handycam-vr - Extract clips from a DVD-VR recorded on a Sony Handycam.

  Copyright © 2026 Adam Livesley <adam@sixones.com>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <getopt.h>
#include <sstream>
#include <vector>

#ifdef HAS_FFMPEG
extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/audio_fifo.h>
    #include <libavutil/opt.h>
    #include <libswresample/swresample.h>
    #include <libswscale/swscale.h>
}
#endif

static constexpr auto VERSION = "0.1.0";
static constexpr auto SECTOR_SIZE = 2048;

struct VOB {
    std::string path;
    uint32_t start = 0;
    uint32_t end = 0;
};

struct Configuration {
    std::string ifoPath;
    std::vector<VOB> vobPaths;
    std::string output;
    std::string prefix;
    bool keepVOBs;
    std::string videoEncoder = "libx264";
    std::string videoEncodingParams = "preset=slow:crf=22";
    std::string audioEncoder = "aac";
};

struct Clip {
    uint16_t chapter;
    uint32_t startSector = 0;
    uint32_t endSector = 0;
    int32_t year, month, day, hour, minute, second;
};

void showHelp(char** argv, const bool failure = false) {
    const char* programName = argv[0] ? argv[0] : "handycam-vr";
    std::ostream& out = failure ? std::cerr : std::cout;

    out << "Usage:" << std::endl
        << "  " << programName << " --dir <dvd-vr-dir> [options]" << std::endl
        << "  " << programName << " --ifo <ifo-file> --vob <vob-file> [--vob <vob-file> ...] [options]" << std::endl << std::endl

        << "Input modes:" << std::endl
        << "  -d, --dir <dir>               DVD-VR directory. Automatically finds VIDEO_RM.IFO and VOB files." << std::endl
        << "  -i, --ifo <file>              Path to VIDEO_RM.IFO." << std::endl
        << "  -b, --vob <file/list>         Path to a VOB file. Can be repeated, or comma-separated." << std::endl << std::endl

        << "Options:" << std::endl
        << "  -o, --output <dir>            Output directory. Defaults to current directory." << std::endl
        << "  -p, --prefix <prefix>         Prefix for generated files. Defaults to 'handycam-'." << std::endl
#ifdef HAS_FFMPEG
        << "  -k, --keep-vobs               Keep the extracted VOB files, rather than deleting them." << std::endl
        << "  -e, --video-encoder <encoder> Specifies the video encoder. Defaults to 'libx264'." << std::endl
        << "  -r, --video-params <params>   Video params to pass to the encoder. Defaults to 'preset=slow:crf=22'." << std::endl
        << "  -a, --audio-encoder <encoder> Specifies the audio encoder. Defaults to 'aac'." << std::endl
#endif
        << "  -h, --help                    Show this help message." << std::endl
        << "  -v, --version                 Show version information." << std::endl << std::endl

        << "Examples:" << std::endl
        << "  " << programName << " --dir /path/to/DVD" << std::endl
        << "  " << programName << " -d /path/to/DVD -o ./clips -p christmas-" << std::endl
        << "  " << programName << " --ifo /path/to/VIDEO_RM.IFO --vob /path/to/VR_MOVIE.VRO" << std::endl
        << "  " << programName << " -i /path/to/VIDEO_RM.IFO -b one.VOB -b two.VOB" << std::endl
        << "  " << programName << " -i /path/to/VIDEO_RM.IFO -b one.VOB,two.VOB" << std::endl;

    std::exit(failure ? EXIT_FAILURE : EXIT_SUCCESS);
}

Configuration parseConfiguration(const int argc, char** argv) {
    static constexpr option options[] =
    {
        {"dir", required_argument, nullptr, 'd'},
        {"ifo", required_argument, nullptr, 'i'},
        {"vob", required_argument, nullptr, 'b'},
        {"output", required_argument, nullptr, 'o'},
        {"prefix", required_argument, nullptr, 'p'},
        {"keep-vobs", no_argument, nullptr, 'k'},
        {"video-encoder", required_argument, nullptr, 'e'},
        {"video-params", required_argument, nullptr, 'r'},
        {"audio-encoder", required_argument, nullptr, 'a'},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, 'v'},
        {nullptr, 0, nullptr, 0}
    };

    Configuration config = {
        .output = ".",
        .prefix = "handycam-",
        .keepVOBs = false
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:i:b:o:p:e:r:a:khv", options, nullptr)) != -1) {
        switch (opt) {
            case 'd': {
                // DVD-VR directory to use as an input
                std::string path = optarg;

                // Find all the VOBs.
                for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::canonical(path) / "VIDEO_TS")) {
                    if (entry.path().extension() == ".VOB") {
                        config.vobPaths.push_back({
                            .path = entry.path().string()
                        });
                    }
                }

                if (!config.ifoPath.empty()) {
                    showHelp(argv, true);
                    return config;
                }

                // Find the IFO.
                config.ifoPath = std::filesystem::canonical(path) / "VIDEO_RM" / "VIDEO_RM.IFO";

                break;
            }
            case 'i': {
                if (!config.ifoPath.empty()) {
                    showHelp(argv, true);
                    return config;
                }

                // Manually specified IFO path.
                config.ifoPath = optarg;
                break;
            }
            case 'b': {
                // VOB paths via comma separated list.
                if (std::string path = optarg; path.contains(",")) {
                    std::stringstream ss(path);
                    std::string item;

                    while (std::getline(ss, item, ',')) {
                        config.vobPaths.push_back({
                            .path = item
                        });
                    }
                } else {
                    // Multiple params just add to the list.
                    config.vobPaths.emplace_back(path);
                }
                break;
            }
            case 'o':
                // Output directory.
                config.output = optarg;
                break;
            case 'p':
                // Prefix to append files with.
                config.prefix = optarg;
                break;
            case 'k':
                config.keepVOBs = true;
                break;
            case 'e':
                config.videoEncoder = optarg;
                break;
            case 'r':
                config.videoEncodingParams = optarg;
                break;
            case 'a':
                config.audioEncoder = optarg;
                break;
            case 'h':
                showHelp(argv, false);
                break;
            case 'v':
                std::cout << "handycam-vr: Version: " << VERSION << std::endl;
                std::cout << "  Written by Adam Livesley <adam@sixones.com>" << std::endl;
                std::exit(EXIT_SUCCESS);
            default:
                showHelp(argv, true);
                break;
        }
    }

    config.output = std::filesystem::canonical(config.output);

    if (!std::filesystem::is_directory(config.output)) {
        std::filesystem::create_directory(config.output);
    }

    if (!std::filesystem::is_regular_file(config.ifoPath)) {
        std::cerr << "ERROR: IFO not found at " << config.ifoPath << std::endl;
        std::exit(EXIT_FAILURE);
    }

    uint32_t nextSector = 0;

    for (auto&[path, start, end] : config.vobPaths) {
        if (!std::filesystem::is_regular_file(path)) {
            std::cerr << "ERROR: VOB not found at " << path << std::endl;
            std::exit(EXIT_FAILURE);
        }

        size_t size = std::filesystem::file_size(path);

        const auto sectorCount = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;

        start = nextSector;
        end = nextSector + sectorCount;

        nextSector = end;
    }

    if (config.vobPaths.empty()) {
        std::cerr << "ERROR: No VOBs specified." << std::endl;
        std::exit(EXIT_FAILURE);
    }

    return config;
}

std::vector<Clip> parseIFO(const Configuration& config) {
    std::ifstream file(config.ifoPath, std::ios::binary);
    if (!file) {
        std::cerr << "ERROR: Could not open " << config.ifoPath << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // Program count is stored at offset 0x800, so we can skip everything before it.
    file.seekg(0x800);

    // Parse the number of clips / chapters / programs.
    unsigned char clipCount;
    file.read(reinterpret_cast<char*>(&clipCount), 1);

    // Move to the program table at offset 0x820.
    file.seekg(0x820);

    std::vector<Clip> clips;

    // Parse the programs, each program (clip) includes the start sector in a 16 byte block.
    for (uint16_t i = 1; i <= clipCount; ++i) {
        unsigned char record[16];
        file.read(reinterpret_cast<char*>(&record), 16);

        Clip clip{
            .chapter = i,
            .startSector = static_cast<uint32_t>(record[4]) << 24 |
                           static_cast<uint32_t>(record[5]) << 16 |
                           static_cast<uint32_t>(record[6]) << 8 |
                           static_cast<uint32_t>(record[7])
        };

        clips.push_back(clip);
    }

    return clips;
}

const VOB* findVOB(const Configuration& config, const uint32_t sector) {
    for (const auto& vob : config.vobPaths) {
        if (sector >= vob.start && sector < vob.end) {
            return &vob;
        }
    }
    return nullptr;
}

const VOB* findVOB(const Configuration& config, const Clip& clip) {
    return findVOB(config, clip.startSector);
}

bool parseRecordedTimestamp(const unsigned char* pack, Clip& clip) {
    // Parse the recorded timestamp (that's in the ARI_DATA payload), it's stored as a packed bit field so we can
    // construct the values by extracting and combining bits.
    clip.year   = (pack[0] << 8 | pack[1]) >> 2;
    clip.month  = (pack[1] & 0x03) << 2 | pack[2] >> 6;
    clip.day    = (pack[2] & 0x3E) >> 1;
    clip.hour   = (pack[2] & 0x01) << 4 | pack[3] >> 4;
    clip.minute = (pack[3] & 0x0F) << 2 | pack[4] >> 6;
    clip.second = pack[4] & 0x3F;

    return clip.year > 0;
}

void parseVOB(const Configuration& config, Clip& clip) {
    const auto vob = findVOB(config, clip);

    if (!vob) {
        std::cerr << "ERROR: No VOB for sector " << clip.startSector << std::endl;
        std::exit(EXIT_FAILURE);
    }

    std::ifstream file(vob->path, std::ios::binary);

    const auto ariOffset = static_cast<int64_t>(clip.startSector + 1 - vob->start) * SECTOR_SIZE;

    unsigned char pack[SECTOR_SIZE];
    file.seekg(ariOffset);

    if (!file.read(reinterpret_cast<char*>(pack), SECTOR_SIZE)) {
        std::cerr << "ERROR: Could not read from " << vob->path << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // Expect MPEG-PS pack start code (0x000001BA) at sector start.
    if (pack[0] != 0 || pack[1] != 0 || pack[2] != 1 || pack[3] != 0xBA) {
        std::cerr << "ERROR: Invalid sector in " << vob->path << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // Skip over the start code (4 bytes), the SCR, mux rate, flags (9 bytes) and stuffing length field (1 byte).
    int position = 14;

    while (position < SECTOR_SIZE - 6) {
        // Expect the PES packet start (0x000001) to parse out the ARI_DATA.
        if (pack[position] != 0 || pack[position + 1] != 0 || pack[position + 2] != 1) {
            break;
        }

        const unsigned char streamID  = pack[position + 3];
        const int packetLength = (pack[position + 4] << 8) | pack[position + 5];

        if (packetLength == 0) {
            break;
        }

        // Check for the MPEG private stream (0xBD) after we've found the PES start.
        if (streamID == 0xBD) {
            const int headerLength = pack[position + 8];

            // Skip the PES header, and check if we have the Sony ARI_DATA marker (0xFF + "ARI_DATA") in the private
            // stream.
            if (const int payloadStart = position + 9 + headerLength; pack[payloadStart] == 0xFF &&
                std::memcmp(&pack[payloadStart + 1], "ARI_DATA", 8) == 0) {
                // Recorded timestamp is stored at a fixed offset (163 bytes) within the Sony ARI_DATA payload, there's
                // likely more interesting data in here (such as the camera data), but haven't been able to find this.
                // The ARI_DATA is not part of the DVD-VR specification and was reversed engineered from the recorded
                // timestamps that the Handycam shows for each clip.
                parseRecordedTimestamp(&pack[payloadStart + 163], clip);
                return;
            }
        }

        // Skip over this packet and the header (start code (3 bytes), stream id (1 byte) and packet length (2 bytes)).
        position += 6 + packetLength;
    }
}

void extractVOB(const Configuration& config, const Clip& clip, const std::string& vobPath) {
    std::cout << "  Extracting clip to " << vobPath << std::endl;

    // Create a temporary MPEG-PS/VOB containing only this clip's sector range.
    std::ofstream out(vobPath, std::ios::binary);

    if (!out) {
        std::cerr << "ERROR: Cannot create " << vobPath << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // 1MB buffer for copying over in chunks.
    std::vector<char> buffer(1 << 20);

    uint32_t sector = clip.startSector;

    while (sector < clip.endSector) {
        // Find which original VOB contains the current sector, they can drift between VOB files.
        const VOB* vob = findVOB(config, sector);
        if (!vob) {
            break;
        }

        // Copy up to the end of the sector, or the end of the current VOB file, if we read up to the end of the VOB
        // file we will fill in the rest from the next VOB file.
        const uint32_t vobEnd = std::min(clip.endSector, vob->end);

        std::ifstream in(vob->path, std::ios::binary);

        // Convert our sector position to something local to this VOB file.
        in.seekg(static_cast<int64_t>(sector - vob->start) * SECTOR_SIZE);

        int64_t remaining = static_cast<int64_t>(vobEnd - sector) * SECTOR_SIZE;

        while (remaining > 0) {
            // Copy over in chunks to save reading the entire thing in memory.
            const auto chunk = static_cast<std::streamsize>(
                std::min(remaining, static_cast<int64_t>(buffer.size()))
            );

            in.read(buffer.data(), chunk);

            const auto got = in.gcount();

            if (got <= 0) {
                break;
            }

            out.write(buffer.data(), got);
            remaining -= got;
        }

        // Move to the next sector range, most likely in the next VOB.
        sector = vobEnd;
    }
}

#ifdef HAS_FFMPEG
void transcodeVOB(const Configuration& config, const Clip& clip, const std::string& vobPath, const std::string& mp4Path) {
    std::cout << "  Transcoding to MP4 at " << mp4Path << std::endl;

    AVFormatContext* inputFormatContext = nullptr;

    av_log_set_level(AV_LOG_ERROR);

    if (avformat_open_input(&inputFormatContext, vobPath.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "ERROR: Cannot open " << vobPath << std::endl;
        std::exit(EXIT_FAILURE);
    }

    avformat_find_stream_info(inputFormatContext, nullptr);

    const int videoStreamID = av_find_best_stream(inputFormatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioStreamID = av_find_best_stream(inputFormatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (videoStreamID < 0) {
        std::cerr << "ERROR: No video stream in " << vobPath << std::endl;
        avformat_close_input(&inputFormatContext);
        std::exit(EXIT_FAILURE);
    }

    const AVCodec* decoderVideoCodec = avcodec_find_decoder(inputFormatContext->streams[videoStreamID]->codecpar->codec_id);
    AVCodecContext* videoDecoderContext = avcodec_alloc_context3(decoderVideoCodec);

    avcodec_parameters_to_context(videoDecoderContext, inputFormatContext->streams[videoStreamID]->codecpar);
    avcodec_open2(videoDecoderContext, decoderVideoCodec, nullptr);

    SwsContext* swsContext = nullptr;

    if (videoDecoderContext->pix_fmt != AV_PIX_FMT_YUV420P) {
        swsContext = sws_getContext(videoDecoderContext->width, videoDecoderContext->height, videoDecoderContext->pix_fmt,
                                videoDecoderContext->width, videoDecoderContext->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    }

    const AVCodec* encoderVideoCodec = avcodec_find_encoder_by_name(config.videoEncoder.c_str());

    if (encoderVideoCodec == nullptr) {
        std::cerr << "ERROR: Cannot find encoder " << config.videoEncoder << std::endl;
        std::exit(EXIT_FAILURE);
    }

    AVCodecContext* videoEncoderContext = avcodec_alloc_context3(encoderVideoCodec);
    videoEncoderContext->width = videoDecoderContext->width;
    videoEncoderContext->height = videoDecoderContext->height;
    videoEncoderContext->pix_fmt = AV_PIX_FMT_YUV420P;
    videoEncoderContext->time_base = {1, 25};
    videoEncoderContext->framerate = {25, 1};

    av_opt_set_from_string(
        videoEncoderContext->priv_data,
        config.videoEncodingParams.c_str(),
        nullptr,
        "=",
        ":"
    );

    AVFormatContext* outputContext = nullptr;
    if (avformat_alloc_output_context2(&outputContext, nullptr, nullptr, mp4Path.c_str()) < 0 || !outputContext) {
        std::cerr << "ERROR: Cannot create output context for " << mp4Path << std::endl;
        std::exit(EXIT_FAILURE);
    }

    if (outputContext->oformat->flags & AVFMT_GLOBALHEADER) {
        videoEncoderContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    avcodec_open2(videoEncoderContext, encoderVideoCodec, nullptr);

    AVStream* outputVideoStream = avformat_new_stream(outputContext, nullptr);

    avcodec_parameters_from_context(outputVideoStream->codecpar, videoEncoderContext);

    outputVideoStream->time_base = videoEncoderContext->time_base;
    
    AVCodecContext* audioDecoderContext = nullptr;
    AVCodecContext* audioEncoderContext = nullptr;
    SwrContext* swrContext = nullptr;
    AVStream* outputAudioStream = nullptr;
    AVAudioFifo* audioFifo = nullptr;

    if (audioStreamID >= 0) {
        const AVCodec* audioDecoderCodec =
            avcodec_find_decoder(inputFormatContext->streams[audioStreamID]->codecpar->codec_id);

        const AVCodec* audioEncoderCodec = avcodec_find_encoder_by_name(config.audioEncoder.c_str());

        if (audioEncoderCodec == nullptr) {
            std::cerr << "ERROR: Cannot find encoder " << config.audioEncoder << std::endl;
            std::exit(EXIT_FAILURE);
        }

        audioDecoderContext = avcodec_alloc_context3(audioDecoderCodec);

        avcodec_parameters_to_context(audioDecoderContext, inputFormatContext->streams[audioStreamID]->codecpar);
        avcodec_open2(audioDecoderContext, audioDecoderCodec, nullptr);

        audioEncoderContext = avcodec_alloc_context3(audioEncoderCodec);
        audioEncoderContext->sample_rate = audioDecoderContext->sample_rate;
        audioEncoderContext->bit_rate = 192000;
        audioEncoderContext->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audioEncoderContext->time_base = {1, audioEncoderContext->sample_rate};

        av_channel_layout_copy(&audioEncoderContext->ch_layout, &audioDecoderContext->ch_layout);

        swr_alloc_set_opts2(
            &swrContext,
            &audioEncoderContext->ch_layout,
            audioEncoderContext->sample_fmt,
            audioEncoderContext->sample_rate,
            &audioDecoderContext->ch_layout,
            audioDecoderContext->sample_fmt,
            audioDecoderContext->sample_rate,
            0,
            nullptr
        );

        swr_init(swrContext);

        if (outputContext->oformat->flags & AVFMT_GLOBALHEADER) {
            audioEncoderContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        avcodec_open2(audioEncoderContext, audioEncoderCodec, nullptr);

        outputAudioStream = avformat_new_stream(outputContext, nullptr);

        avcodec_parameters_from_context(outputAudioStream->codecpar, audioEncoderContext);

        outputAudioStream->time_base = audioEncoderContext->time_base;

        audioFifo = av_audio_fifo_alloc(
            audioEncoderContext->sample_fmt,
            audioEncoderContext->ch_layout.nb_channels,
            audioEncoderContext->frame_size
        );
    }

    const std::string recordedDateTime = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}",
        clip.year, clip.month, clip.day, clip.hour, clip.minute, clip.second);

    av_dict_set(&outputContext->metadata, "creation_time", recordedDateTime.c_str(), 0);
    av_dict_set(&outputContext->metadata, "date", recordedDateTime.c_str(), 0);

    if (avio_open(&outputContext->pb, mp4Path.c_str(), AVIO_FLAG_WRITE) < 0) {
        std::cerr << "ERROR: Cannot open output file " << mp4Path << std::endl;
        std::exit(EXIT_FAILURE);
    }

    if (avformat_write_header(outputContext, nullptr) < 0) {
        std::cerr << "ERROR: Failed to write header in output." << std::endl;
        std::exit(EXIT_FAILURE);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* scaled = av_frame_alloc();

    int64_t videoPTSBase = AV_NOPTS_VALUE;
    int64_t nextAudioPTS = 0;

    auto drainEncoder = [&](AVCodecContext* encoderContext, const AVStream* outputStream) {
        AVPacket* encPkt = av_packet_alloc();

        while (avcodec_receive_packet(encoderContext, encPkt) == 0) {
            encPkt->stream_index = outputStream->index;

            av_packet_rescale_ts(encPkt, encoderContext->time_base, outputStream->time_base);
            av_interleaved_write_frame(outputContext, encPkt);
        }

        av_packet_free(&encPkt);
    };

    while (av_read_frame(inputFormatContext, pkt) >= 0) {
        if (pkt->stream_index == videoStreamID) {
            avcodec_send_packet(videoDecoderContext, pkt);

            while (avcodec_receive_frame(videoDecoderContext, frame) == 0) {
                if (videoPTSBase == AV_NOPTS_VALUE) {
                    videoPTSBase = frame->best_effort_timestamp;
                }

                const int64_t ticks = frame->best_effort_timestamp - videoPTSBase;
                if (ticks < 0) {
                    av_frame_unref(frame);
                    continue;
                }

                AVFrame* toEncode = frame;

                if (swsContext) {
                    av_frame_unref(scaled);

                    scaled->width  = videoDecoderContext->width;
                    scaled->height = videoDecoderContext->height;
                    scaled->format = AV_PIX_FMT_YUV420P;
                    av_frame_get_buffer(scaled, 0);

                    sws_scale(swsContext, frame->data, frame->linesize, 0, frame->height,
                              scaled->data, scaled->linesize);

                    toEncode = scaled;
                }

                toEncode->pts = av_rescale_q(ticks, inputFormatContext->streams[videoStreamID]->time_base, videoEncoderContext->time_base);

                avcodec_send_frame(videoEncoderContext, toEncode);

                drainEncoder(videoEncoderContext, outputVideoStream);

                av_frame_unref(frame);
            }
        } else if (audioDecoderContext && pkt->stream_index == audioStreamID) {
            avcodec_send_packet(audioDecoderContext, pkt);

            while (avcodec_receive_frame(audioDecoderContext, frame) == 0) {
                const int outSamples = av_rescale_rnd(
                    swr_get_delay(swrContext, audioDecoderContext->sample_rate) + frame->nb_samples,
                    audioEncoderContext->sample_rate,
                    audioDecoderContext->sample_rate,
                    AV_ROUND_UP
                );

                uint8_t** converted = nullptr;

                av_samples_alloc_array_and_samples(
                    &converted,
                    nullptr,
                    audioEncoderContext->ch_layout.nb_channels,
                    outSamples,
                    audioEncoderContext->sample_fmt,
                    0
                );

                const int convertedSamples = swr_convert(
                    swrContext,
                    converted,
                    outSamples,
                    frame->extended_data,
                    frame->nb_samples
                );

                if (av_audio_fifo_realloc(
                    audioFifo,
                    av_audio_fifo_size(audioFifo) + convertedSamples
                ) < 0) {
                    std::cerr << "ERROR: Failed to reallocate FiFo." << std::endl;
                }

                av_audio_fifo_write(
                    audioFifo,
                    reinterpret_cast<void**>(converted),
                    convertedSamples
                );

                av_freep(&converted[0]);
                av_freep(&converted);

                while (av_audio_fifo_size(audioFifo) >= audioEncoderContext->frame_size) {
                    AVFrame* audioFrame = av_frame_alloc();

                    audioFrame->nb_samples = audioEncoderContext->frame_size;
                    audioFrame->format = audioEncoderContext->sample_fmt;
                    audioFrame->sample_rate = audioEncoderContext->sample_rate;

                    av_channel_layout_copy(&audioFrame->ch_layout, &audioEncoderContext->ch_layout);

                    av_frame_get_buffer(audioFrame, 0);

                    av_audio_fifo_read(
                        audioFifo,
                        reinterpret_cast<void**>(audioFrame->data),
                        audioEncoderContext->frame_size
                    );

                    audioFrame->pts = nextAudioPTS;
                    nextAudioPTS += audioFrame->nb_samples;

                    avcodec_send_frame(audioEncoderContext, audioFrame);

                    drainEncoder(audioEncoderContext, outputAudioStream);

                    av_frame_free(&audioFrame);
                }

                av_frame_unref(frame);
            }
        }

        av_packet_unref(pkt);
    }

    avcodec_send_packet(videoDecoderContext, nullptr);

    while (avcodec_receive_frame(videoDecoderContext, frame) == 0) {
        if (videoPTSBase == AV_NOPTS_VALUE) {
            videoPTSBase = frame->best_effort_timestamp;
        }

        if (const int64_t ticks = frame->best_effort_timestamp - videoPTSBase; ticks >= 0) {
            frame->pts = av_rescale_q(ticks, inputFormatContext->streams[videoStreamID]->time_base, videoEncoderContext->time_base);

            avcodec_send_frame(videoEncoderContext, frame);
            drainEncoder(videoEncoderContext, outputVideoStream);
        }

        av_frame_unref(frame);
    }

    avcodec_send_frame(videoEncoderContext, nullptr);
    drainEncoder(videoEncoderContext, outputVideoStream);

    if (audioDecoderContext) {
        avcodec_send_packet(audioDecoderContext, nullptr);

        while (avcodec_receive_frame(audioDecoderContext, frame) == 0) {
            const int outSamples = av_rescale_rnd(
                swr_get_delay(swrContext, audioDecoderContext->sample_rate) + frame->nb_samples,
                audioEncoderContext->sample_rate,
                audioDecoderContext->sample_rate,
                AV_ROUND_UP
            );

            uint8_t** converted = nullptr;

            av_samples_alloc_array_and_samples(
                &converted,
                nullptr,
                audioEncoderContext->ch_layout.nb_channels,
                outSamples,
                audioEncoderContext->sample_fmt,
                0
            );

            const int convertedSamples = swr_convert(
                swrContext,
                converted,
                outSamples,
                frame->extended_data,
                frame->nb_samples
            );

            if (convertedSamples > 0) {
                if (av_audio_fifo_realloc(
                    audioFifo,
                    av_audio_fifo_size(audioFifo) + convertedSamples
                ) < 0) {
                    std::cerr << "ERROR: Failed to reallocate FiFo." << std::endl;
                }

                av_audio_fifo_write(
                    audioFifo,
                    reinterpret_cast<void**>(converted),
                    convertedSamples
                );
            }

            av_freep(&converted[0]);
            av_freep(&converted);
            av_frame_unref(frame);
        }

        while (av_audio_fifo_size(audioFifo) > 0) {
            const int samplesToEncode = std::min(
                av_audio_fifo_size(audioFifo),
                audioEncoderContext->frame_size
            );

            AVFrame* audioFrame = av_frame_alloc();

            audioFrame->nb_samples = samplesToEncode;
            audioFrame->format = audioEncoderContext->sample_fmt;
            audioFrame->sample_rate = audioEncoderContext->sample_rate;

            av_channel_layout_copy(&audioFrame->ch_layout, &audioEncoderContext->ch_layout);

            av_frame_get_buffer(audioFrame, 0);

            av_audio_fifo_read(
                audioFifo,
                reinterpret_cast<void**>(audioFrame->data),
                samplesToEncode
            );

            audioFrame->pts = nextAudioPTS;
            nextAudioPTS += audioFrame->nb_samples;

            avcodec_send_frame(audioEncoderContext, audioFrame);

            drainEncoder(audioEncoderContext, outputAudioStream);

            av_frame_free(&audioFrame);
        }

        avcodec_send_frame(audioEncoderContext, nullptr);

        drainEncoder(audioEncoderContext, outputAudioStream);
    }

    av_write_trailer(outputContext);

    av_frame_free(&scaled);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    if (swsContext) {
        sws_freeContext(swsContext);
    }

    if (audioFifo) {
        av_audio_fifo_free(audioFifo);
    }

    if (swrContext) {
        swr_free(&swrContext);
    }

    avcodec_free_context(&audioDecoderContext);
    avcodec_free_context(&audioEncoderContext);

    avcodec_free_context(&videoDecoderContext);
    avcodec_free_context(&videoEncoderContext);
    avformat_close_input(&inputFormatContext);
    avio_closep(&outputContext->pb);
    avformat_free_context(outputContext);
}
#endif

void extractClip(const Configuration& config, const Clip& clip) {
    // Path for the clip `output-dir/prefix-count-YYYYMMDD-HHMMSS`.
    const std::string path = std::format("{}/{}{}-{:04}{:02}{:02}-{:02}{:02}{:02}",
        config.output, config.prefix,
        clip.chapter,
        clip.year, clip.month, clip.day, clip.hour, clip.minute, clip.second
    );

    extractVOB(config, clip, path + ".vob");

#ifdef HAS_FFMPEG
    transcodeVOB(config, clip, path + ".vob", path + ".mp4");

    if (!config.keepVOBs) {
        std::cout << "  Removing temporary VOB" << std::endl;
        if (std::error_code errorCode; !std::filesystem::remove(path + ".vob", errorCode)) {
            if (errorCode) {
                std::cerr << "ERROR: Failed to delete file: " << errorCode.message() << std::endl;
            }
        }
    }
#endif
}

int main(const int argc, char* argv[]) {
    const auto config = parseConfiguration(argc, argv);

    std::string vobPaths;

    for (const auto& vob : config.vobPaths) {
        if (!vobPaths.empty()) {
            vobPaths += ", ";
        }

        vobPaths += vob.path;
    }

    std::cout << "IFO: " << config.ifoPath << std::endl
              << "VOBs: " << vobPaths << std::endl
              << "Prefix: " << config.prefix << std::endl
              << "Output: " << config.output << std::endl
              << std::endl;

    // Parse the IFO for the number of clips and sector starts.
    std::vector<Clip> clips = parseIFO(config);

    // Parse all the VOB's and populate the recorded time for each clip.
    for (auto& clip : clips) {
        parseVOB(config, clip);
    }

    // Calculate end of sector by looking at the next sector start.
    for (size_t i = 0; i + 1 < clips.size(); ++i) {
        clips[i].endSector = clips[i + 1].startSector;
    }

    // Calculate the last clip's end of sector by placing it at the end of the last VOB file.
    if (!clips.empty()) {
        clips.back().endSector = config.vobPaths.back().end;
    }

    for (size_t i = 0; i < clips.size(); ++i) {
        auto& clip = clips[i];

        std::cout << std::format(
            "Clip {} Recorded at {:02}/{:02}/{:04} {:02}:{:02}:{:02} (Sector at {} to {})",
            clip.chapter,
            clip.day,
            clip.month,
            clip.year,
            clip.hour,
            clip.minute,
            clip.second,
            clip.startSector,
            clip.endSector
        ) << std::endl;

        extractClip(config, clip);
    }

    return 0;
}