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
#include <sstream>

#include "Log.hpp"
#ifdef __ANDROID__
#include "Android.hpp"
#endif

namespace {

bool DefaultUseVulkan()
{
#ifdef SUPERFLIGHT_USE_VULKAN
	return true;
#else
	return false;
#endif
}

void ApplyRendererArguments(bool& useVulkan, const char* arguments)
{
	std::istringstream stream(arguments ? arguments : "");
	std::string argument;
	while (stream >> argument)
	{
		if (argument == "--vulkan")
			useVulkan = true;
		else if (argument == "--opengl")
			useVulkan = false;
	}
}

int RunGame(bool useVulkan)
{
	fe::LogSetTag("SuperFlight");
	fe::Log("SuperFlight starting");

	try {
		fe::LogToFile("Creating SuperFlight game instance...");

		fe::XRGameOptions options(1200, 800);
		options.useVulkan = useVulkan;
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

}

int main(int argc, char* argv[]) {
	bool useVulkan = DefaultUseVulkan();
	for (int i = 1; i < argc; ++i)
		ApplyRendererArguments(useVulkan, argv[i]);
	return RunGame(useVulkan);
}

#ifdef _WIN32

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
	bool useVulkan = DefaultUseVulkan();
	ApplyRendererArguments(useVulkan, lpCmdLine);
	return RunGame(useVulkan);
}

#endif
