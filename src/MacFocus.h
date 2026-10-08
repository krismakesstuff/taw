#pragma once

// macOS: the app taw's terminal runs in (Terminal, iTerm2, Ghostty...), brought to the front so
// keys go back to the terminal, as Console::focusTerminal does on Windows.
namespace taw::macFocus
{
void activateTerminal();

// The process id of the app in front (that has the keyboard), for tests.
int frontmostApp();
} // namespace taw::macFocus
