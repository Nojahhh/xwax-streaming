/*
 * Illustrate clipping of characters
 */

#include <stdio.h>
#include <SDL.h>
#include <SDL_ttf.h>

SDL_Window *window;
SDL_Renderer *ren;
TTF_Font *font;

void draw(void)
{
    const SDL_Color fg = { 255, 255, 255, 255 }, bg = { 0, 0, 0, 255 };
    SDL_Surface *rendered;
    SDL_Texture *tex;
    SDL_Rect dest, source;

    rendered = TTF_RenderText_Shaded(font, "Track at 101.0 BPM a0a0", fg, bg);

    tex = SDL_CreateTextureFromSurface(ren, rendered);

    source.x = 0;
    source.y = 0;
    source.w = rendered->w;
    source.h = rendered->h;

    dest.x = 0;
    dest.y = 0;
    dest.w = rendered->w;
    dest.h = rendered->h;

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, &source, &dest);
    SDL_RenderPresent(ren);

    SDL_DestroyTexture(tex);
    SDL_FreeSurface(rendered);
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fputs("usage: test-ttf <font-file>\n", stderr);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) == -1)
        abort();

    if (TTF_Init() == -1)
        abort();

    font = TTF_OpenFont(argv[1], 15);
    if (font == NULL)
        abort();

#ifdef TTF_HINTING_NONE
    TTF_SetFontHinting(font, TTF_HINTING_NONE);
#endif
    TTF_SetFontKerning(font, 1);

    window = SDL_CreateWindow("TTF Test",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        400, 200, 0);
    if (window == NULL)
        abort();

    ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (ren == NULL)
        abort();

    for (;;) {
        SDL_Event event;

        if (SDL_WaitEvent(&event) < 0)
            abort();

        switch (event.type) {
        case SDL_QUIT:
            goto done;
        }

        draw();
    }
done:

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(window);
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
