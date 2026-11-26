#include <dsu/dsu_client.h>
#include <av/av_codecs.h>

#include <SDL.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_WIDTH 640
#define VIDEO_HEIGHT 360
#define VIDEO_FPS 30
#define VIDEO_BITRATE_KBPS 1500
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_SAMPLES 480
#define GRID_COLUMNS 32
#define GRID_ROWS 18
#define CELL_SIZE 20
#define MAX_SNAKE_LENGTH (GRID_COLUMNS * GRID_ROWS)
#define ENCODED_FRAME_CAPACITY (256 * 1024)
#define OPUS_PACKET_CAPACITY 1200

typedef struct {
    int x;
    int y;
} GridPoint;

typedef enum {
    DIRECTION_UP,
    DIRECTION_DOWN,
    DIRECTION_LEFT,
    DIRECTION_RIGHT,
} Direction;

typedef struct {
    DsuClient client;
    AvCodecs *codecs;
    DsuAvCapabilities capabilities;
    DsuAvSessionSettings desired_settings;
    bool capabilities_received;
    bool session_started;
    uint64_t next_session_attempt_us;
    bool force_keyframe;
    uint64_t media_start_us;
    uint64_t last_video_us;
    uint64_t next_audio_us;
    bool audio_started;
    uint64_t audio_encoded_packets;
    uint64_t audio_sent_packets;
    uint64_t audio_send_failures;
    uint64_t next_audio_log_us;
    uint8_t *framebuffer;
    int16_t audio_pcm[AUDIO_SAMPLES * AUDIO_CHANNELS];
    uint8_t encoded_video[ENCODED_FRAME_CAPACITY];
    uint8_t encoded_audio[OPUS_PACKET_CAPACITY];
    double audio_phase;
    bool tone_active;
    uint64_t tone_until_us;
    GridPoint snake[MAX_SNAKE_LENGTH];
    size_t snake_length;
    GridPoint food;
    Direction direction;
    Direction pending_direction;
    bool paused;
    bool button_a_was_down;
    bool game_over;
    uint64_t next_move_us;
    uint32_t score;
} SnakeGame;

static uint64_t monotonic_us(void)
{
    return av_codecs_monotonic_us();
}

static void put_pixel(SnakeGame *game, int x, int y, uint8_t red,
    uint8_t green, uint8_t blue)
{
    if (x < 0 || y < 0 || x >= VIDEO_WIDTH || y >= VIDEO_HEIGHT)
        return;
    uint8_t *pixel = game->framebuffer +
        ((size_t)y * VIDEO_WIDTH + (size_t)x) * 4;
    pixel[0] = red;
    pixel[1] = green;
    pixel[2] = blue;
    pixel[3] = 255;
}

static void fill_rectangle(SnakeGame *game, int x, int y, int width,
    int height, uint8_t red, uint8_t green, uint8_t blue)
{
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column)
            put_pixel(game, column, row, red, green, blue);
    }
}

static bool opposite_directions(Direction first, Direction second)
{
    return (first == DIRECTION_UP && second == DIRECTION_DOWN) ||
        (first == DIRECTION_DOWN && second == DIRECTION_UP) ||
        (first == DIRECTION_LEFT && second == DIRECTION_RIGHT) ||
        (first == DIRECTION_RIGHT && second == DIRECTION_LEFT);
}

static void place_food(SnakeGame *game)
{
    for (;;) {
        GridPoint candidate = {
            .x = rand() % GRID_COLUMNS,
            .y = rand() % GRID_ROWS,
        };
        bool occupied = false;
        for (size_t i = 0; i < game->snake_length; ++i) {
            if (game->snake[i].x == candidate.x &&
                game->snake[i].y == candidate.y) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            game->food = candidate;
            return;
        }
    }
}

static void reset_game(SnakeGame *game)
{
    game->snake_length = 5;
    for (size_t i = 0; i < game->snake_length; ++i) {
        game->snake[i] = (GridPoint){
            .x = GRID_COLUMNS / 2 - (int)i,
            .y = GRID_ROWS / 2,
        };
    }
    game->direction = DIRECTION_RIGHT;
    game->pending_direction = DIRECTION_RIGHT;
    game->paused = false;
    game->game_over = false;
    game->score = 0;
    place_food(game);
    game->next_move_us = monotonic_us() + 120000;
}

static void move_snake(SnakeGame *game)
{
    if (game->paused || game->game_over)
        return;
    if (!opposite_directions(game->direction, game->pending_direction))
        game->direction = game->pending_direction;

    GridPoint next = game->snake[0];
    if (game->direction == DIRECTION_UP)
        --next.y;
    else if (game->direction == DIRECTION_DOWN)
        ++next.y;
    else if (game->direction == DIRECTION_LEFT)
        --next.x;
    else
        ++next.x;

    if (next.x < 0)
        next.x = GRID_COLUMNS - 1;
    else if (next.x >= GRID_COLUMNS)
        next.x = 0;
    if (next.y < 0)
        next.y = GRID_ROWS - 1;
    else if (next.y >= GRID_ROWS)
        next.y = 0;

    bool ate_food = next.x == game->food.x && next.y == game->food.y;
    size_t collision_length = game->snake_length - (ate_food ? 0 : 1);
    for (size_t i = 0; i < collision_length; ++i) {
        if (game->snake[i].x == next.x && game->snake[i].y == next.y) {
            game->game_over = true;
            return;
        }
    }

    if (ate_food) {
        if (game->snake_length < MAX_SNAKE_LENGTH)
            ++game->snake_length;
        game->score += 10;
        game->tone_active = true;
        game->tone_until_us = monotonic_us() + 90000;
        if (game->snake_length == MAX_SNAKE_LENGTH)
            game->game_over = true;
        else
            place_food(game);
    }
    for (size_t i = game->snake_length - 1; i > 0; --i)
        game->snake[i] = game->snake[i - 1];
    game->snake[0] = next;
}

static void render_game(SnakeGame *game)
{
    uint8_t background[VIDEO_WIDTH * 4];
    for (int x = 0; x < VIDEO_WIDTH; ++x) {
        background[x * 4] = 10;
        background[x * 4 + 1] = 18;
        background[x * 4 + 2] = 28;
        background[x * 4 + 3] = 255;
    }
    for (int y = 0; y < VIDEO_HEIGHT; ++y) {
        memcpy(game->framebuffer + (size_t)y * sizeof(background),
            background, sizeof(background));
    }

    for (int x = 0; x <= GRID_COLUMNS; ++x)
        fill_rectangle(game, x * CELL_SIZE, 0, 1, VIDEO_HEIGHT, 20, 32, 46);
    for (int y = 0; y <= GRID_ROWS; ++y)
        fill_rectangle(game, 0, y * CELL_SIZE, VIDEO_WIDTH, 1, 20, 32, 46);

    fill_rectangle(game, game->food.x * CELL_SIZE + 4,
        game->food.y * CELL_SIZE + 4, CELL_SIZE - 8, CELL_SIZE - 8,
        245, 82, 82);
    for (size_t i = 0; i < game->snake_length; ++i) {
        uint8_t green = i == 0 ? 238 : 170;
        fill_rectangle(game, game->snake[i].x * CELL_SIZE + 2,
            game->snake[i].y * CELL_SIZE + 2, CELL_SIZE - 4, CELL_SIZE - 4,
            75, green, 120);
    }
    if (game->paused)
        fill_rectangle(game, VIDEO_WIDTH / 2 - 24, VIDEO_HEIGHT / 2 - 34,
            16, 68, 255, 211, 92);
    if (game->paused)
        fill_rectangle(game, VIDEO_WIDTH / 2 + 8, VIDEO_HEIGHT / 2 - 34,
            16, 68, 255, 211, 92);
    if (game->game_over) {
        int margin = 120;
        fill_rectangle(game, margin, VIDEO_HEIGHT / 2 - 10,
            VIDEO_WIDTH - margin * 2, 20, 190, 45, 60);
    }
}

static void on_controller(void *context, const DsuControllerState *state,
    uint32_t packet_number)
{
    (void)packet_number;
    SnakeGame *game = context;
    if (state->button_masks[0] & 0x80)
        game->pending_direction = DIRECTION_LEFT;
    else if (state->button_masks[0] & 0x40)
        game->pending_direction = DIRECTION_DOWN;
    else if (state->button_masks[0] & 0x20)
        game->pending_direction = DIRECTION_RIGHT;
    else if (state->button_masks[0] & 0x10)
        game->pending_direction = DIRECTION_UP;
    else if (state->left_stick_x < 72)
        game->pending_direction = DIRECTION_LEFT;
    else if (state->left_stick_x > 184)
        game->pending_direction = DIRECTION_RIGHT;
    else if (state->left_stick_y > 184)
        game->pending_direction = DIRECTION_UP;
    else if (state->left_stick_y < 72)
        game->pending_direction = DIRECTION_DOWN;

    bool button_a_down = (state->button_masks[1] & 0x20) != 0;
    if (button_a_down && !game->button_a_was_down) {
        if (game->game_over)
            reset_game(game);
        else
            game->paused = !game->paused;
    }
    game->button_a_was_down = button_a_down;
}

static void on_capabilities(void *context,
    const DsuAvCapabilities *capabilities)
{
    SnakeGame *game = context;
    game->capabilities = *capabilities;
    game->capabilities_received = true;
    if (!(capabilities->flags & DSU_AV_CAP_VIDEO) ||
        !(capabilities->flags & DSU_AV_CAP_AUDIO) ||
        capabilities->video_codec != DSU_AV_CODEC_H264 ||
        capabilities->audio_codec != DSU_AV_CODEC_OPUS ||
        capabilities->video_width < VIDEO_WIDTH ||
        capabilities->video_height < VIDEO_HEIGHT ||
        capabilities->video_fps < VIDEO_FPS) {
        fprintf(stderr, "Controller does not support H.264 and Opus.\n");
        return;
    }

    DsuAvSessionSettings settings = {
        .video_codec = DSU_AV_CODEC_H264,
        .audio_codec = DSU_AV_CODEC_OPUS,
        .video_width = VIDEO_WIDTH,
        .video_height = VIDEO_HEIGHT,
        .video_fps = VIDEO_FPS,
        .video_bitrate_kbps = VIDEO_BITRATE_KBPS,
        .audio_sample_rate_div100 = AUDIO_SAMPLE_RATE / 100,
        .audio_channels = AUDIO_CHANNELS,
        .audio_frame_duration_01ms = 100,
        .payload_limit = 1100,
    };
    settings.video_bitrate_kbps =
        settings.video_bitrate_kbps <= capabilities->video_bitrate_kbps
        ? settings.video_bitrate_kbps : capabilities->video_bitrate_kbps;
    settings.payload_limit =
        settings.payload_limit <= capabilities->payload_limit
        ? settings.payload_limit : capabilities->payload_limit;
    if (settings.audio_channels > capabilities->audio_channels ||
        settings.audio_sample_rate_div100 >
            capabilities->audio_sample_rate_div100 ||
        capabilities->audio_frame_duration_01ms > 100) {
        fprintf(stderr, "Controller audio capabilities are incompatible.\n");
        return;
    }
    if (!dsu_client_start_media(&game->client, &settings))
        game->next_session_attempt_us = monotonic_us() + 1000000;
    game->desired_settings = settings;
}

static void on_media_session(void *context, bool started, uint8_t status,
    const DsuAvSessionSettings *settings)
{
    SnakeGame *game = context;
    if (!started) {
        fprintf(stderr, "DSU-AV session rejected (status %u).\n",
            (unsigned)status);
        game->next_session_attempt_us = monotonic_us() + 1000000;
        return;
    }
    game->codecs = av_codecs_create(settings);
    if (!game->codecs) {
        fprintf(stderr, "Could not initialize snake media codecs.\n");
        dsu_client_stop_media(&game->client, 3);
        return;
    }
    game->media_start_us = monotonic_us();
    game->last_video_us = 0;
    game->next_audio_us = game->media_start_us;
    game->audio_started = false;
    game->audio_encoded_packets = 0;
    game->audio_sent_packets = 0;
    game->audio_send_failures = 0;
    game->next_audio_log_us = monotonic_us() + 1000000;
    game->force_keyframe = true;
    game->session_started = true;
    printf("Streaming snake at %ux%u and %u FPS.\n",
        (unsigned)settings->video_width, (unsigned)settings->video_height,
        (unsigned)settings->video_fps);
}

static void on_media_stopped(void *context, uint8_t reason)
{
    SnakeGame *game = context;
    printf("Media session stopped (reason %u).\n", (unsigned)reason);
    game->session_started = false;
    game->next_session_attempt_us = monotonic_us() + 1000000;
    av_codecs_destroy(game->codecs);
    game->codecs = NULL;
}

static void on_keyframe(void *context, uint32_t last_sequence,
    uint16_t lost_packets, uint8_t reason)
{
    (void)last_sequence;
    (void)lost_packets;
    (void)reason;
    ((SnakeGame *)context)->force_keyframe = true;
}

static void produce_audio(SnakeGame *game, uint64_t timestamp_us)
{
    double frequency = game->tone_active ? 660.0 : 220.0;
    if (timestamp_us > game->tone_until_us)
        game->tone_active = false;
    double amplitude = game->tone_active ? 18000.0 : 12000.0;
    for (int i = 0; i < AUDIO_SAMPLES; ++i) {
        double sample = sin(game->audio_phase) * amplitude;
        game->audio_pcm[i * AUDIO_CHANNELS] = (int16_t)sample;
        game->audio_pcm[i * AUDIO_CHANNELS + 1] = (int16_t)sample;
        game->audio_phase += 2.0 * M_PI * frequency / AUDIO_SAMPLE_RATE;
        if (game->audio_phase >= 2.0 * M_PI)
            game->audio_phase -= 2.0 * M_PI;
    }
    int length = av_codecs_encode_opus(game->codecs, game->audio_pcm,
        AUDIO_SAMPLES, game->encoded_audio, OPUS_PACKET_CAPACITY);
    if (length <= 0) {
        ++game->audio_send_failures;
        return;
    }
    ++game->audio_encoded_packets;
    if (dsu_client_send_audio(&game->client,
            timestamp_us - game->media_start_us,
            game->audio_started ? 0 : 0x01,
            game->encoded_audio, (uint16_t)length)) {
        game->audio_started = true;
        ++game->audio_sent_packets;
    } else {
        ++game->audio_send_failures;
    }
    uint64_t now = monotonic_us();
    if (now >= game->next_audio_log_us) {
        printf("Snake audio: encoded=%llu sent=%llu failed=%llu "
            "last_size=%d active=%d.\n",
            (unsigned long long)game->audio_encoded_packets,
            (unsigned long long)game->audio_sent_packets,
            (unsigned long long)game->audio_send_failures, length,
            dsu_client_media_active(&game->client) ? 1 : 0);
        fflush(stdout);
        game->next_audio_log_us = now + 1000000;
    }
}

int main(int argc, char **argv)
{
    const char *server_address = argc > 1 ? argv[1] : "127.0.0.1";
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("DSU Snake Client",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        VIDEO_WIDTH, VIDEO_HEIGHT, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
        VIDEO_WIDTH, VIDEO_HEIGHT);
    if (!window || !renderer || !texture) {
        fprintf(stderr, "Could not create SDL display: %s\n", SDL_GetError());
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SnakeGame game = {0};
    game.framebuffer = malloc(VIDEO_WIDTH * VIDEO_HEIGHT * 4);
    if (!game.framebuffer) {
        fprintf(stderr, "Could not allocate the video framebuffer.\n");
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    reset_game(&game);
    render_game(&game);
    DsuClientCallbacks callbacks = {
        .controller_state = on_controller,
        .capabilities = on_capabilities,
        .media_session = on_media_session,
        .media_stopped = on_media_stopped,
        .keyframe_requested = on_keyframe,
    };
    DsuClientConfig config = {
        .server_address = server_address,
        .server_port = DSU_PORT,
        .controller_slot = 0,
    };
    if (!dsu_client_init(&game.client, &config, &callbacks, &game)) {
        fprintf(stderr, "Could not create a DSU client for %s:%u.\n",
            server_address, (unsigned)DSU_PORT);
        free(game.framebuffer);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    dsu_client_request_capabilities(&game.client);
    printf("Connecting to DSU controller at %s:%u.\n",
        server_address, (unsigned)DSU_PORT);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN &&
                    event.key.keysym.sym == SDLK_ESCAPE))
                running = false;
            else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_LEFT:
                case SDLK_a:
                    game.pending_direction = DIRECTION_LEFT;
                    break;
                case SDLK_DOWN:
                case SDLK_s:
                    game.pending_direction = DIRECTION_DOWN;
                    break;
                case SDLK_RIGHT:
                case SDLK_d:
                    game.pending_direction = DIRECTION_RIGHT;
                    break;
                case SDLK_UP:
                case SDLK_w:
                    game.pending_direction = DIRECTION_UP;
                    break;
                case SDLK_SPACE:
                    if (game.game_over)
                        reset_game(&game);
                    else
                        game.paused = !game.paused;
                    break;
                default:
                    break;
                }
            }
        }
        dsu_client_poll(&game.client);

        uint64_t now = monotonic_us();
        if (game.capabilities_received && !game.session_started &&
            !dsu_client_media_active(&game.client) &&
            now >= game.next_session_attempt_us) {
            game.next_session_attempt_us = now + 1000000;
            dsu_client_start_media(&game.client, &game.desired_settings);
        }
        while (now >= game.next_move_us) {
            move_snake(&game);
            game.next_move_us += 120000;
        }

        if (game.session_started) {
            if (now >= game.next_audio_us) {
                produce_audio(&game, game.next_audio_us);
                if (now - game.next_audio_us > 100000)
                    game.next_audio_us = now + 10000;
                else
                    game.next_audio_us += 10000;
            }
            if (now - game.last_video_us >= 1000000ULL / VIDEO_FPS) {
                render_game(&game);
                bool is_keyframe = false;
                int encoded = av_codecs_encode_rgba(game.codecs,
                    game.framebuffer, VIDEO_WIDTH * 4,
                    now - game.media_start_us, game.force_keyframe,
                    &is_keyframe, game.encoded_video,
                    ENCODED_FRAME_CAPACITY);
                game.force_keyframe = false;
                game.last_video_us = now;
                if (encoded > 0) {
                    uint8_t flags = is_keyframe ? 0x01 : 0;
                    dsu_client_send_video(&game.client,
                        now - game.media_start_us, flags,
                        game.encoded_video, (uint32_t)encoded);
                }
            }
        }

        SDL_UpdateTexture(texture, NULL, game.framebuffer,
            VIDEO_WIDTH * 4);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_Delay(4);
    }

    dsu_client_close(&game.client);
    av_codecs_destroy(game.codecs);
    free(game.framebuffer);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
