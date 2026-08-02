#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <SDL2/SDL.h>

class VideoPlayer {
public:
    explicit VideoPlayer(const std::string& filename)
        : filename_(filename) {
        openVideo();
        initializeSDL();
    }

    ~VideoPlayer() {
        cleanup();
    }

    void run() {
        std::cout << "Playing: " << filename_ << "\n";
        std::cout << "Controls:\n";
        std::cout << "  Space      Pause / Resume\n";
        std::cout << "  Q / Escape Quit\n";

        performanceFrequency_ = static_cast<double>(
            SDL_GetPerformanceFrequency()
        );

        nextFrameTime_ = currentTime();

        while (!quit_) {
            int ret = av_read_frame(formatContext_, packet_);

            if (ret == AVERROR_EOF) {
                break;
            }

            checkFFmpeg(ret, "av_read_frame");

            if (packet_->stream_index == videoStreamIndex_) {
                decodePacket(packet_);
            }

            av_packet_unref(packet_);
        }

        // Flush decoder.
        if (!quit_) {
            decodePacket(nullptr);
        }
    }

private:
    // FFmpeg
    AVFormatContext* formatContext_ = nullptr;
    AVCodecContext* codecContext_ = nullptr;

    AVFrame* decodedFrame_ = nullptr;
    AVFrame* yuvFrame_ = nullptr;
    AVPacket* packet_ = nullptr;

    SwsContext* swsContext_ = nullptr;

    int videoStreamIndex_ = -1;

    int videoWidth_ = 0;
    int videoHeight_ = 0;

    double fps_ = 25.0;
    double frameDuration_ = 1.0 / 25.0;

    // SDL
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;

    // Playback
    bool quit_ = false;
    bool paused_ = false;

    double performanceFrequency_ = 1.0;
    double nextFrameTime_ = 0.0;

    std::string filename_;

private:
    static void checkFFmpeg(int ret, const char* operation) {
        if (ret >= 0) {
            return;
        }

        char errorBuffer[AV_ERROR_MAX_STRING_SIZE];

        av_strerror(
            ret,
            errorBuffer,
            sizeof(errorBuffer)
        );

        throw std::runtime_error(
            std::string(operation) +
            " failed: " +
            errorBuffer
        );
    }

    double currentTime() const {
        return static_cast<double>(
            SDL_GetPerformanceCounter()
        ) / performanceFrequency_;
    }

    void openVideo() {
        int ret = avformat_open_input(
            &formatContext_,
            filename_.c_str(),
            nullptr,
            nullptr
        );

        checkFFmpeg(ret, "avformat_open_input");

        ret = avformat_find_stream_info(
            formatContext_,
            nullptr
        );

        checkFFmpeg(
            ret,
            "avformat_find_stream_info"
        );

        // Find the best video stream.
        const AVCodec* decoder = nullptr;

        videoStreamIndex_ = av_find_best_stream(
            formatContext_,
            AVMEDIA_TYPE_VIDEO,
            -1,
            -1,
            &decoder,
            0
        );

        if (videoStreamIndex_ < 0) {
            throw std::runtime_error(
                "Could not find a video stream"
            );
        }

        AVStream* videoStream =
            formatContext_->streams[videoStreamIndex_];

        if (!decoder) {
            throw std::runtime_error(
                "Could not find a decoder"
            );
        }

        // Create codec context.
        codecContext_ = avcodec_alloc_context3(decoder);

        if (!codecContext_) {
            throw std::runtime_error(
                "Could not allocate codec context"
            );
        }

        ret = avcodec_parameters_to_context(
            codecContext_,
            videoStream->codecpar
        );

        checkFFmpeg(
            ret,
            "avcodec_parameters_to_context"
        );

        ret = avcodec_open2(
            codecContext_,
            decoder,
            nullptr
        );

        checkFFmpeg(
            ret,
            "avcodec_open2"
        );

        videoWidth_ = codecContext_->width;
        videoHeight_ = codecContext_->height;

        // Get approximate frame rate.
        AVRational frameRate =
            av_guess_frame_rate(
                formatContext_,
                videoStream,
                nullptr
            );

        if (frameRate.num > 0 && frameRate.den > 0) {
            fps_ = av_q2d(frameRate);
        }

        if (fps_ <= 0.0) {
            fps_ = 25.0;
        }

        frameDuration_ = 1.0 / fps_;

        std::cout
            << "Resolution: "
            << videoWidth_
            << "x"
            << videoHeight_
            << "\n";

        std::cout
            << "FPS: "
            << fps_
            << "\n";

        // Allocate frames and packets.
        decodedFrame_ = av_frame_alloc();
        yuvFrame_ = av_frame_alloc();
        packet_ = av_packet_alloc();

        if (!decodedFrame_ ||
            !yuvFrame_ ||
            !packet_) {
            throw std::runtime_error(
                "Could not allocate FFmpeg structures"
            );
        }

        // Our rendering format will always be YUV420P.
        yuvFrame_->format = AV_PIX_FMT_YUV420P;
        yuvFrame_->width = videoWidth_;
        yuvFrame_->height = videoHeight_;

        ret = av_frame_get_buffer(
            yuvFrame_,
            32
        );

        checkFFmpeg(
            ret,
            "av_frame_get_buffer"
        );

        // Convert whatever the decoder produces
        // into YUV420P.
        swsContext_ = sws_getContext(
            videoWidth_,
            videoHeight_,
            codecContext_->pix_fmt,

            videoWidth_,
            videoHeight_,
            AV_PIX_FMT_YUV420P,

            SWS_BILINEAR,

            nullptr,
            nullptr,
            nullptr
        );

        if (!swsContext_) {
            throw std::runtime_error(
                "Could not create SwsContext"
            );
        }
    }

    void initializeSDL() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            throw std::runtime_error(
                std::string("SDL_Init failed: ") +
                SDL_GetError()
            );
        }

        SDL_SetHint(
            SDL_HINT_RENDER_SCALE_QUALITY,
            "linear"
        );

        window_ = SDL_CreateWindow(
            "C++ Video Player",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            960,
            540,
            SDL_WINDOW_RESIZABLE
        );

        if (!window_) {
            throw std::runtime_error(
                std::string("SDL_CreateWindow failed: ") +
                SDL_GetError()
            );
        }

        // Try accelerated renderer first.
        renderer_ = SDL_CreateRenderer(
            window_,
            -1,
            SDL_RENDERER_ACCELERATED |
            SDL_RENDERER_PRESENTVSYNC
        );

        // Fallback to software rendering.
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(
                window_,
                -1,
                SDL_RENDERER_SOFTWARE
            );
        }

        if (!renderer_) {
            throw std::runtime_error(
                std::string("SDL_CreateRenderer failed: ") +
                SDL_GetError()
            );
        }

        // SDL IYUV corresponds to planar YUV420.
        texture_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            videoWidth_,
            videoHeight_
        );

        if (!texture_) {
            throw std::runtime_error(
                std::string("SDL_CreateTexture failed: ") +
                SDL_GetError()
            );
        }
    }

    void pollEvents() {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit_ = true;
                return;
            }

            if (event.type == SDL_KEYDOWN &&
                event.key.repeat == 0) {

                switch (event.key.keysym.sym) {
                    case SDLK_ESCAPE:
                    case SDLK_q:
                        quit_ = true;
                        break;

                    case SDLK_SPACE:
                        paused_ = !paused_;

                        if (!paused_) {
                            // Restart timing from now.
                            nextFrameTime_ = currentTime();
                        }

                        break;

                    default:
                        break;
                }
            }
        }
    }

    void waitForNextFrame() {
        while (!quit_) {
            pollEvents();

            if (paused_) {
                SDL_Delay(10);

                // Do not accumulate a huge delay
                // while paused.
                nextFrameTime_ = currentTime();

                continue;
            }

            double now = currentTime();
            double remaining =
                nextFrameTime_ - now;

            if (remaining <= 0.0) {
                return;
            }

            // Wake up frequently so that keyboard
            // input remains responsive.
            Uint32 delayMs = static_cast<Uint32>(
                std::min(
                    remaining * 1000.0,
                    5.0
                )
            );

            SDL_Delay(delayMs);
        }
    }

    void renderFrame() {
        int ret = av_frame_make_writable(
            yuvFrame_
        );

        checkFFmpeg(
            ret,
            "av_frame_make_writable"
        );

        // Convert decoded frame -> YUV420P.
        int scaledHeight = sws_scale(
            swsContext_,

            decodedFrame_->data,
            decodedFrame_->linesize,

            0,
            videoHeight_,

            yuvFrame_->data,
            yuvFrame_->linesize
        );

        if (scaledHeight <= 0) {
            throw std::runtime_error(
                "sws_scale failed"
            );
        }

        // Copy YUV planes into SDL texture.
        if (SDL_UpdateYUVTexture(
                texture_,
                nullptr,

                yuvFrame_->data[0],
                yuvFrame_->linesize[0],

                yuvFrame_->data[1],
                yuvFrame_->linesize[1],

                yuvFrame_->data[2],
                yuvFrame_->linesize[2]
            ) != 0) {

            throw std::runtime_error(
                std::string(
                    "SDL_UpdateYUVTexture failed: "
                ) +
                SDL_GetError()
            );
        }

        int windowWidth;
        int windowHeight;

        SDL_GetWindowSize(
            window_,
            &windowWidth,
            &windowHeight
        );

        // Preserve video aspect ratio.
        double videoAspect =
            static_cast<double>(videoWidth_) /
            static_cast<double>(videoHeight_);

        int destinationWidth = windowWidth;

        int destinationHeight =
            static_cast<int>(
                destinationWidth /
                videoAspect
            );

        if (destinationHeight > windowHeight) {
            destinationHeight = windowHeight;

            destinationWidth =
                static_cast<int>(
                    destinationHeight *
                    videoAspect
                );
        }

        SDL_Rect destination;

        destination.w = destinationWidth;
        destination.h = destinationHeight;

        destination.x =
            (windowWidth - destinationWidth) / 2;

        destination.y =
            (windowHeight - destinationHeight) / 2;

        // Black background.
        SDL_SetRenderDrawColor(
            renderer_,
            0,
            0,
            0,
            255
        );

        SDL_RenderClear(renderer_);

        // Draw video.
        SDL_RenderCopy(
            renderer_,
            texture_,
            nullptr,
            &destination
        );

        SDL_RenderPresent(renderer_);
    }

    void decodePacket(AVPacket* packet) {
        int ret = avcodec_send_packet(
            codecContext_,
            packet
        );

        checkFFmpeg(
            ret,
            "avcodec_send_packet"
        );

        while (!quit_) {
            ret = avcodec_receive_frame(
                codecContext_,
                decodedFrame_
            );

            if (ret == AVERROR(EAGAIN) ||
                ret == AVERROR_EOF) {
                return;
            }

            checkFFmpeg(
                ret,
                "avcodec_receive_frame"
            );

            waitForNextFrame();

            if (quit_) {
                return;
            }

            renderFrame();

            // Schedule the next video frame.
            nextFrameTime_ += frameDuration_;
        }
    }

    void cleanup() {
        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }

        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }

        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }

        if (SDL_WasInit(SDL_INIT_VIDEO)) {
            SDL_Quit();
        }

        if (swsContext_) {
            sws_freeContext(swsContext_);
            swsContext_ = nullptr;
        }

        if (decodedFrame_) {
            av_frame_free(&decodedFrame_);
        }

        if (yuvFrame_) {
            av_frame_free(&yuvFrame_);
        }

        if (packet_) {
            av_packet_free(&packet_);
        }

        if (codecContext_) {
            avcodec_free_context(&codecContext_);
        }

        if (formatContext_) {
            avformat_close_input(&formatContext_);
        }
    }
};

int main(int argc, char* argv[]) {
    std::string vidpath = argc == 2 ? argv[1] : "vid.webm";
   
    try {
        VideoPlayer player(vidpath);
        player.run();
    }
    catch (const std::exception& e) {
        std::cerr
            << "Error: "
            << e.what()
            << "\n";

        return 1;
    }

    return 0;
}