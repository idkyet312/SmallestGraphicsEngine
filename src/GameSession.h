#ifndef GAME_SESSION_H
#define GAME_SESSION_H

#include <algorithm>

enum class GameScreen {
    MainMenu,
    Level1,
    LevelEditor,
    WinScreen
};

class GameSession {
public:
    GameScreen Screen() const { return screen_; }
    void SetScreen(GameScreen screen) { screen_ = screen; }

    bool IsSceneScreen() const {
        return screen_ == GameScreen::Level1 ||
               screen_ == GameScreen::LevelEditor;
    }

    void ResetTimer(bool running) {
        elapsedSeconds_ = 0.0f;
        timerRunning_ = running;
    }
    void StartTimer() { timerRunning_ = true; }
    void StopTimer() { timerRunning_ = false; }
    bool TimerRunning() const { return timerRunning_; }
    float ElapsedSeconds() const { return elapsedSeconds_; }

    void Tick(float frameDelta) {
        if (timerRunning_)
            elapsedSeconds_ += std::clamp(frameDelta, 0.0f, 0.25f);
    }

private:
    GameScreen screen_ = GameScreen::MainMenu;
    float elapsedSeconds_ = 0.0f;
    bool timerRunning_ = false;
};

#endif
