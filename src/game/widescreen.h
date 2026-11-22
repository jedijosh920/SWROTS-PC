#pragma once

namespace swrots::game {

// True widescreen (Widescreen=2, d3d/d3d.h): the renderer's view is widened at the sides so that the game
// draws, and does not cull, what the wide picture shows beside its 4:3 frame. At every boot, after the
// image is loaded.
void InstallWidescreen();

// Whether the loading screen is up (from its set-up to its release, while a level loads): its picture keeps
// its 4:3 shape with black at the sides (d3d/draw.cpp).
bool LoadingScreenUp();

// The movies (Sofdec .sfd, MPEG-1 video): the game draws a movie's picture in the middle of its 640x480 frame
// (the film's, 640x272, with black above and below). Noted when one is opened (kernel/file.cpp): the size of its
// picture, from the video's sequence header. MoviePicture gives the last one's (false if none was read).
void NoteMovieOpened(const wchar_t* ntPath);
bool MoviePicture(int& width, int& height);

// The front end: the shell up (it stays up in a level too, for the pause menu), no level set up in this boot and
// no level loading. Its background movies cover the width; the film's movies (a level's, played as it loads or in
// it) fill it as Placement::Movie (d3d/d3d.h).
bool InFrontEnd();

} // namespace swrots::game
