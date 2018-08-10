#include <stdio.h>
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

global int SCREEN_WIDTH = 800;
global int SCREEN_HEIGHT = 600;
global bool running;
global SDL_Window* window;
global SDL_Surface* surface;
global SDL_Renderer* renderer;
global double dt_for_frame;
global double time = 0;
global TTF_Font* font;
char message[64];

double notes[] = {
	73.416, 77.782, // Drop D & D#
	82.407, 87.307, 92.499, 97.999, 103.826, 110, 116.541, 123.471, 130.813, 138.591, 146.832, 155.563, 164.814 // First octave
};

intern void generate_audio(void* userdata, uint8_t* stream, int len)
{
	double sample_rate = 48000;
	int num_samples = 4096;

	local_persist int note_index = 0;
	local_persist double note_time = 1.0;
	note_time -=  num_samples / sample_rate;
	double base_freq = notes[note_index];
	local_persist double freq = base_freq;
	if (note_time <= 0)
	{
		note_index++;
		WrapValue(0, note_index, ArrayLen(notes) - 1);
		note_time = 1.0;
		base_freq = notes[note_index];
		freq = base_freq;
	}

	local_persist double phase = 0;
	double chirp_time = 1.0;
	double chirp_amount = 0;	
	double freq_inc = chirp_amount / (sample_rate * chirp_time);
	float volume = 0.5f;

	float* sample = (float*)stream;
	for (int i = 0; i < 4096; i++)
	{
		freq += freq_inc;
		WrapValue(base_freq, freq, base_freq + chirp_amount);
		double phase_inc = freq / sample_rate;
		float val = volume * (float)sin(2.0 * M_PI * phase);
		*sample++ = val;
		*sample++ = val;
		phase += phase_inc;
		//sample_time += time_delta;
	}
	snprintf(message, ArrayLen(message), "%0.6f - %0.6f, note_time=%0.06f, i=%d", freq, freq_inc, note_time, note_index);
}

intern bool init()
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
	want.freq = 48000;
	want.format = AUDIO_F32;
	want.channels = 2;
	want.samples = 4096;
	want.callback = generate_audio;

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

intern void handle_event(SDL_Event* e)
{

}

intern void update()
{

}

intern void render()
{
	SDL_RenderClear(renderer);

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
	if (!init())
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
		}

		uint64_t end_counter = SDL_GetPerformanceCounter();
		uint64_t ticks = end_counter - start_counter;
		start_counter = end_counter;
		dt_for_frame = (double)ticks / (double)counter_freq;
		time += dt_for_frame;

		update();
		render();
		SDL_Delay(1);
		
	}

	return 0;
}