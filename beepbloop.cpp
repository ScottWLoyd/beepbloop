#include <stdio.h>
#include <string.h>
#include <assert.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <SDL.h>
#include <SDL_ttf.h>

#define intern static
#define global static
#define local_persist static

#define ArrayLen(a) (sizeof((a))/sizeof((a)[0]))

#define WrapValue(min,val,max) (((val)<(min))?(val)=(max)-((min)-(val)):((val)>(max))?(val)=(min)+((val)-(max)):(val)=(val))

intern void sdl_message(const char* func, const char* msg)
{
	printf("SDL error(%s): %s\n", func, msg);
	__debugbreak();
}

#define SDL_ERROR(f) sdl_message(f, SDL_GetError())
#define SDL_WARN(f, m) sdl_message(f, m)

global int SCREEN_WIDTH = 1600;
global int SCREEN_HEIGHT = 900;
global bool running;
global SDL_Window* window;
global SDL_Surface* surface;
global SDL_Renderer* renderer;
global double dt_for_frame;
global double time = 0;
global TTF_Font* font;
global uint8_t* stream_data;
global uint32_t stream_len;
global char message[64];

float notes_frequencies[] = {
	73.416f, 77.782f, // Drop D & D#
	82.407f, 87.307f, 92.499f, 97.999f, 103.826f, 110, 116.541f, 123.471f, 130.813f, 138.591f, 146.832f, 155.563f, // First octave
	164.814f, 174.614f, 184.997f, 195.998f, 207.652f, 220, 233.082f, 246.942f, 261.626f, 277.183f, 293.665f, 311.127f, 
	329.628f, 349.228f, 369.994f, 391.995f, 415.305f, 440, 466.164f, 493.883f, 523.251f, 554.365f, 587.33f, 622.254f, 
	659.255f, 698.456f, 739.989f, 783.991f, 830.609f, 880, 932.328f, 987.767f, 1046.502f, 1108.731f, 1174.659f, 1244.508f,
};

struct Note {
	float freq;
	float duration;
	float amplitude;
};

enum FilterType {
	Filter_VolumeLogDamping,
	Filter_VolumeTriangleWave,
	Filter_VolumeOneOverTSquared,
};

struct Filter {
	FilterType type;
	float time_between_samples;
	union {
		struct {
			float gamma;
			float t0;
		} log;
		struct {
			float half_period;
			float min;
			float max;
		} triangle;
	};
};

struct AudioFormat {
	float sample_rate;
	int samples_in_buffer;
};

struct SampleData {
	float* samples;
	uint32_t num_samples;
	uint32_t sample_index;
};

struct Sound {
	uint32_t sample_index;
	float start_t;
	float duration;
};

#define MAX_SOUNDS 256
#define MAX_NOTES 16
struct EngineData {
	bool pause;
	AudioFormat fmt;

	Sound sounds[MAX_SOUNDS];
	uint32_t num_sounds;

	SampleData notes[MAX_NOTES];
	uint32_t num_notes;
};

intern void apply_filter(Filter filter, float* samples, uint32_t num_samples)
{
	switch (filter.type)
	{
	case Filter_VolumeLogDamping: {
		float t = 0;
		for (uint32_t i = 0; i < num_samples; i++)
		{
			samples[i] *= powf((float)M_E, -filter.log.gamma * (t - filter.log.t0));
			t += filter.time_between_samples;
		}
	} break;
	case Filter_VolumeTriangleWave: {
		float t = 0;
		int last_iter = 0;
		float slope = (filter.triangle.max - filter.triangle.min) / filter.triangle.half_period;
		for (uint32_t i = 0; i < num_samples; i++)
		{
			int iter = (int)(t / filter.triangle.half_period);
			if (iter != last_iter)
			{
				slope *= -1;
				last_iter = iter;
			}
			float rel_t = t - (last_iter * filter.triangle.half_period);
			if (iter % 2 == 0)
			{
				//rel_t = filter.triangle.half_period - rel_t;
			}
			float val = samples[i];
			samples[i] = val * slope * rel_t + filter.triangle.min * val;
			t += filter.time_between_samples;
		}
	} break;
	case Filter_VolumeOneOverTSquared: {
		float t = 0;
		for (uint32_t i = 0; i < num_samples; i++)
		{
			if (t > 0)
			{
				samples[i] /= t * t;
			}
			t += filter.time_between_samples;
		}
	} break;
	default: assert(0);
	}
}

void generate_audio(void* userdata, uint8_t* stream, int len);

intern bool init(EngineData* engine)
{
	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_VIDEO | SDL_INIT_TIMER))
	{
		SDL_ERROR("SDL_Init");
		return false;
	}

	if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"))
	{
		SDL_WARN("SDL_SetHint", SDL_GetError());
	}

	window = SDL_CreateWindow("BeepBloop", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
	if (!window)
	{
		SDL_ERROR("SDL_CreateWindow");
		return false;
	}

	surface = SDL_GetWindowSurface(window);
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer)
	{
		SDL_ERROR("SDL_CreateRenderer");
		return false;
	}

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

	// Audio
	SDL_AudioSpec want, have;
	SDL_AudioDeviceID dev;
	SDL_memset(&want, 0, sizeof(want));
	want.freq = (int)engine->fmt.sample_rate;
	want.format = AUDIO_F32;
	want.channels = 2;
	want.samples = (int)engine->fmt.samples_in_buffer;
	want.callback = generate_audio;
	want.userdata = engine;

	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
	if (dev == 0)
	{
		SDL_ERROR("SDL_OpenAudioDevice");
	}
	else
	{
		if (have.format != want.format)
		{
			SDL_WARN("SDL_OpenAudioDevice", "Float32 Audio format not available.");
		}
		SDL_PauseAudioDevice(dev, 0); // Start playing audio
	}

	// Text
	if (TTF_Init())
	{
		SDL_ERROR("TTF_Init");
		return false;
	}
	font = TTF_OpenFont("c:\\windows\\fonts\\arial.ttf", 16);
	if (!font)
	{
		SDL_WARN("TTF_OpenFont", SDL_GetError());
		return false;
	}

	return true;
}


intern void generate_audio(void* userdata, uint8_t* stream, int len)
{
	EngineData* engine = (EngineData*)userdata;
	float* sample = (float*)stream;
	int num_samples = len / (2 * sizeof(float));
	for (int i = 0; i < num_samples; i++)
	{
		if (engine->data.sample_index < engine->data.num_samples && !engine->pause)
		{
			float raw_sample = engine->data.samples[engine->data.sample_index++];
			*sample++ = raw_sample;
			*sample++ = raw_sample;
		}
		else
		{
			*sample++ = 0;
			*sample++ = 0;
		}
	}

	if (!stream_data)
	{
		stream_data = (uint8_t*)malloc(len);
	}
	memcpy(stream_data, stream, len);
	stream_len = len;

	//snprintf(message, ArrayLen(message), "%f - %d", phase_values[engine->data.sample_index], engine->data.sample_index);
}

global const int num_harmonics = 15;
//global float* harmonic_samples[num_harmonics];

intern void synthesize_note(Note* n, EngineData* engine)
{	
	for (uint32_t i = 0; i < engine->data.num_samples; i++)
	{
		engine->data.samples[i] = 0;
	}

	size_t bytes = sizeof(float)*engine->data.num_samples;

	float amps[num_harmonics] = {
		1, 2, 0.95f, 0.95f, 0.45f, 2, 0.95f, 0.95f, 0.95f, 0.45f, 2, 0.95f, 0.95f, 0.45f, 2
	};

	float amplitude = n->amplitude;
	for (int harmonic = 0; harmonic < num_harmonics; harmonic++)
	{
		//harmonic_samples[harmonic] = (float*)malloc(bytes);

		float freq = n->freq * (harmonic + 1);
		//float amplitude = n->amplitude * (1.0f / (harmonic + 1));
		amplitude *= amps[harmonic];
		float phase = 0;
		float phase_inc = freq / engine->fmt.sample_rate;
		for (uint32_t i = 0; i < engine->data.num_samples; i++)
		{
			float val = amplitude * sinf(2.0f * (float)M_PI * phase);
			engine->data.samples[i] += val;
			//harmonic_samples[harmonic][i] = val;
			//harmonic_samples[harmonic][i] = engine->data.samples[i];
			phase += phase_inc;			
		}
	}
	
	// TODO(scott): fix this so it doesn't spike at 1 sec
	//Filter xsq_val = { Filter_VolumeOneOverTSquared, 1.0f / engine->fmt.sample_rate };
	//apply_filter(xsq_val, engine->data.samples, engine->data.num_samples);
	
	Filter tri_vol = { Filter_VolumeTriangleWave };
	tri_vol.triangle.half_period = 0.25f;
	tri_vol.triangle.min = 0.95f;
	tri_vol.triangle.max = 1;
	tri_vol.time_between_samples = 1.0f / engine->fmt.sample_rate;
	apply_filter(tri_vol, engine->data.samples, engine->data.num_samples);
		
	tri_vol.triangle.half_period = 0.6f;
	tri_vol.triangle.min = 0.9f;
	tri_vol.triangle.max = 1;
	tri_vol.time_between_samples = 1.0f / engine->fmt.sample_rate;
	apply_filter(tri_vol, engine->data.samples, engine->data.num_samples);

	tri_vol.triangle.half_period = 0.1f;
	tri_vol.triangle.min = 0.85f;
	tri_vol.triangle.max = 1.;
	apply_filter(tri_vol, engine->data.samples, engine->data.num_samples);

	Filter log_val = { Filter_VolumeLogDamping, 1.0f / engine->fmt.sample_rate };
	log_val.log.gamma = 0.6f;
	log_val.log.t0 = -2;
	apply_filter(log_val, engine->data.samples, engine->data.num_samples);
}

intern void handle_event(SDL_Event* e)
{

}

intern void render(EngineData* engine)
{
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);


	SampleData* data = &engine->data;
	int h_base = SCREEN_HEIGHT / 3;
	SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
	int x = SCREEN_WIDTH * data->sample_index / data->num_samples;
	int y1 = 0;
	int y2 = h_base * 2;
	SDL_RenderDrawLine(renderer, x, y1, x, y2);

#if 0
	SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
	int y_b = SCREEN_HEIGHT / num_harmonics; 
	int y_inc = y_b;
	y_b -= 0.5 * y_b;	
	float amplitude = 0.5;
	for (int h = 0; h < num_harmonics; h++)
	{
		uint32_t start = 0;
		uint32_t end = start + 480000 - 1;
		assert(end < data->num_samples);
		for (uint32_t i = start; i < end; i++)
		{
			int x = SCREEN_WIDTH * (i - start) / (end - start);
			int y = (int)((float)y_b + harmonic_samples[h][i] * (float)y_inc * amplitude);
			SDL_RenderDrawPoint(renderer, x, y);
		}
		y_b += y_inc;
	}
#endif

#if 1
	int h_base2 = SCREEN_HEIGHT / 2;
	SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
	for (uint32_t i = 0; i < data->num_samples; i++)
	{
		float val = data->samples[i];
		int x = SCREEN_WIDTH * i / data->num_samples;
		int y = (int)((float)h_base2 + (float)h_base2 * val * 0.05);
		SDL_RenderDrawPoint(renderer, x, y);
	}
#endif

#if 0

	// Render the current buffer
	SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
	float* curr_sample = (float*)stream_data;
	uint32_t num_samples = stream_len / sizeof(float);
	for (uint32_t i = 0; i < num_samples; i++)
	{
		float val = *curr_sample++;
		int x = SCREEN_WIDTH * i / num_samples;
		int y = (int)((float)h_base2 + (float)h_base * val);
		SDL_RenderDrawPoint(renderer, x, y);
	}
#endif

	SDL_Color white = { 255, 255, 255, 255 };
	SDL_Surface* surf = TTF_RenderText_Blended(font, message, white);
	SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surf);
	int width, height;
	TTF_SizeText(font, message, &width, &height);
	SDL_Rect rect;
	rect.x = 0;
	rect.y = 0;
	rect.w = width;
	rect.h = height;
	SDL_RenderCopy(renderer, texture, NULL, &rect);
	SDL_FreeSurface(surf);
	SDL_DestroyTexture(texture);

	SDL_RenderPresent(renderer);
}

int main(int argc, char** argv)
{
	EngineData engine = { 0 };
	engine.fmt.samples_in_buffer = 4096;
	engine.fmt.sample_rate = 48000;

	Note n = { 0 };
	n.freq = notes_frequencies[2]; // low E
	n.amplitude = 1;
	n.duration = 10;

	engine.data.num_samples = (int)(engine.fmt.sample_rate * n.duration);
	engine.data.samples = (float*)malloc(sizeof(float) * engine.data.num_samples);
	synthesize_note(&n, &engine);
	
	if (!init(&engine))
	{
		return 0;
	}

	uint64_t counter_freq = SDL_GetPerformanceFrequency();
	uint64_t start_counter = SDL_GetPerformanceCounter();
	
	SDL_Event event;
	running = true;
	while (running)
	{
		while (SDL_PollEvent(&event))
		{
			handle_event(&event);

			if (event.type == SDL_QUIT)
			{
				running = false;
			}
			if (event.type == SDL_KEYDOWN)
			{
				engine.data.sample_index = 0;
				//engine.pause = true;
			}
		}

		uint64_t end_counter = SDL_GetPerformanceCounter();
		uint64_t ticks = end_counter - start_counter;
		start_counter = end_counter;
		dt_for_frame = (double)ticks / (double)counter_freq;
		time += dt_for_frame;
		
		render(&engine);
		SDL_Delay(1);
		
	}

	return 0;
}