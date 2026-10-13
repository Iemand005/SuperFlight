#include "XRGame.hpp"
#if defined(_WIN32)
#elif !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
#include <X11/Xlib.h>
#endif
#include <string>
#include "SuperFlight.hpp"

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>
#endif

#include <exception>

#include "Log.hpp"
#ifdef __ANDROID__
#include "Android.hpp"
#endif

int main(int argc, char* argv[]) {

	fe::LogSetTag("SuperFlight");
	fe::Log("SuperFlight starting");

	try {
		fe::LogToFile("Creating SuperFlight game instance...");

		fe::XRGameOptions options(1200, 800);
		options.useVulkan = false;
#ifdef __ANDROID__
		fe::AndroidSetupAssets("resources");
		options.launchVR = true;
#endif
		SuperFlight game(options);

		fe::LogToFile("Running game...");

		game.Run();

		fe::LogToFile("Game exited normally");
	} catch (const std::exception& e) {
		fe::LogToFile(std::string("Exception caught: ") + e.what());
	} catch (...) {
		fe::LogToFile("Unknown exception caught");
	}

	return 0;
}

#ifdef _WIN32

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
	return main(0, nullptr);
}

#endif
