#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

namespace app {

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s", kGame.title, IMAGE_XBE, date);
    LOGR("Platform: Android arm64, SDL %d", SDL_GetVersion());
    Mode desk = Desktop();
    LOGR("Display: %ux%u", desk.width, desk.height);
    LogSettings();
}

}  // namespace app
