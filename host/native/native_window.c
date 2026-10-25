/* Interactive PC window for the gl profile (ISAAC_NATIVE_WINDOW=1): a GLFW
 * window with an EGL/GLES 3.0 context instead of the off-screen pbuffer.
 * Keys reach the game as Win32 messages (virtual key + set-1 scancode, the
 * codes explore.mjs uses); the first GLFW gamepad answers the game's own
 * XInput path. Closing the window leaves through WM_QUIT. */
#ifndef __SWITCH__
#include <GLFW/glfw3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void isaac_input_key(uint32_t vk, uint32_t scancode, int extended, int down);
void isaac_native_request_quit(void);

static GLFWwindow *g_win;

static const struct { int glfw; uint8_t vk, sc, ext; } KEYMAP[] = {
    {GLFW_KEY_ENTER, 0x0D, 0x1C, 0}, {GLFW_KEY_KP_ENTER, 0x0D, 0x1C, 1},
    {GLFW_KEY_ESCAPE, 0x1B, 0x01, 0}, {GLFW_KEY_SPACE, 0x20, 0x39, 0}, {GLFW_KEY_TAB, 0x09, 0x0F, 0},
    {GLFW_KEY_BACKSPACE, 0x08, 0x0E, 0}, {GLFW_KEY_DELETE, 0x2E, 0x53, 1}, {GLFW_KEY_INSERT, 0x2D, 0x52, 1},
    {GLFW_KEY_HOME, 0x24, 0x47, 1}, {GLFW_KEY_END, 0x23, 0x4F, 1},
    {GLFW_KEY_PAGE_UP, 0x21, 0x49, 1}, {GLFW_KEY_PAGE_DOWN, 0x22, 0x51, 1},
    {GLFW_KEY_UP, 0x26, 0x48, 1}, {GLFW_KEY_DOWN, 0x28, 0x50, 1},
    {GLFW_KEY_LEFT, 0x25, 0x4B, 1}, {GLFW_KEY_RIGHT, 0x27, 0x4D, 1},
    {GLFW_KEY_LEFT_SHIFT, 0x10, 0x2A, 0}, {GLFW_KEY_RIGHT_SHIFT, 0x10, 0x36, 0},
    {GLFW_KEY_LEFT_CONTROL, 0x11, 0x1D, 0}, {GLFW_KEY_RIGHT_CONTROL, 0x11, 0x1D, 1},
    {GLFW_KEY_LEFT_ALT, 0x12, 0x38, 0},
    {GLFW_KEY_A, 0x41, 0x1E, 0}, {GLFW_KEY_B, 0x42, 0x30, 0}, {GLFW_KEY_C, 0x43, 0x2E, 0},
    {GLFW_KEY_D, 0x44, 0x20, 0}, {GLFW_KEY_E, 0x45, 0x12, 0}, {GLFW_KEY_F, 0x46, 0x21, 0},
    {GLFW_KEY_G, 0x47, 0x22, 0}, {GLFW_KEY_H, 0x48, 0x23, 0}, {GLFW_KEY_I, 0x49, 0x17, 0},
    {GLFW_KEY_J, 0x4A, 0x24, 0}, {GLFW_KEY_K, 0x4B, 0x25, 0}, {GLFW_KEY_L, 0x4C, 0x26, 0},
    {GLFW_KEY_M, 0x4D, 0x32, 0}, {GLFW_KEY_N, 0x4E, 0x31, 0}, {GLFW_KEY_O, 0x4F, 0x18, 0},
    {GLFW_KEY_P, 0x50, 0x19, 0}, {GLFW_KEY_Q, 0x51, 0x10, 0}, {GLFW_KEY_R, 0x52, 0x13, 0},
    {GLFW_KEY_S, 0x53, 0x1F, 0}, {GLFW_KEY_T, 0x54, 0x14, 0}, {GLFW_KEY_U, 0x55, 0x16, 0},
    {GLFW_KEY_V, 0x56, 0x2F, 0}, {GLFW_KEY_W, 0x57, 0x11, 0}, {GLFW_KEY_X, 0x58, 0x2D, 0},
    {GLFW_KEY_Y, 0x59, 0x15, 0}, {GLFW_KEY_Z, 0x5A, 0x2C, 0},
    {GLFW_KEY_0, 0x30, 0x0B, 0}, {GLFW_KEY_1, 0x31, 0x02, 0}, {GLFW_KEY_2, 0x32, 0x03, 0},
    {GLFW_KEY_3, 0x33, 0x04, 0}, {GLFW_KEY_4, 0x34, 0x05, 0}, {GLFW_KEY_5, 0x35, 0x06, 0},
    {GLFW_KEY_6, 0x36, 0x07, 0}, {GLFW_KEY_7, 0x37, 0x08, 0}, {GLFW_KEY_8, 0x38, 0x09, 0},
    {GLFW_KEY_9, 0x39, 0x0A, 0},
    {GLFW_KEY_GRAVE_ACCENT, 0xC0, 0x29, 0}, {GLFW_KEY_MINUS, 0xBD, 0x0C, 0}, {GLFW_KEY_EQUAL, 0xBB, 0x0D, 0},
    {GLFW_KEY_LEFT_BRACKET, 0xDB, 0x1A, 0}, {GLFW_KEY_RIGHT_BRACKET, 0xDD, 0x1B, 0},
    {GLFW_KEY_BACKSLASH, 0xDC, 0x2B, 0}, {GLFW_KEY_SEMICOLON, 0xBA, 0x27, 0},
    {GLFW_KEY_APOSTROPHE, 0xDE, 0x28, 0}, {GLFW_KEY_COMMA, 0xBC, 0x33, 0},
    {GLFW_KEY_PERIOD, 0xBE, 0x34, 0}, {GLFW_KEY_SLASH, 0xBF, 0x35, 0},
    {GLFW_KEY_F1, 0x70, 0x3B, 0}, {GLFW_KEY_F2, 0x71, 0x3C, 0}, {GLFW_KEY_F3, 0x72, 0x3D, 0},
    {GLFW_KEY_F4, 0x73, 0x3E, 0}, {GLFW_KEY_F5, 0x74, 0x3F, 0}, {GLFW_KEY_F6, 0x75, 0x40, 0},
    {GLFW_KEY_F7, 0x76, 0x41, 0}, {GLFW_KEY_F8, 0x77, 0x42, 0}, {GLFW_KEY_F9, 0x78, 0x43, 0},
    {GLFW_KEY_F10, 0x79, 0x44, 0}, {GLFW_KEY_F11, 0x7A, 0x57, 0}, {GLFW_KEY_F12, 0x7B, 0x58, 0},
};

static void on_key(GLFWwindow *w, int key, int scancode, int action, int mods) {
    (void)w; (void)scancode; (void)mods;
    if (action == GLFW_REPEAT) return;           /* the game reads key state, not repeats */
    for (size_t i = 0; i < sizeof KEYMAP / sizeof KEYMAP[0]; ++i) {
        if (KEYMAP[i].glfw != key) continue;
        isaac_input_key(KEYMAP[i].vk, KEYMAP[i].sc, KEYMAP[i].ext, action == GLFW_PRESS);
        return;
    }
}

int isaac_native_window_open(int w, int h) {
    if (!glfwInit()) return 0;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    g_win = glfwCreateWindow(w, h, "Isaac (native, browsing lift)", NULL, NULL);
    if (!g_win) { glfwTerminate(); return 0; }
    glfwMakeContextCurrent(g_win);
    glfwSwapInterval(0);            /* paced by ISAAC_NATIVE_FPS: NVIDIA EGL blocks ~1 s per swap on a hidden window */
    glfwSetKeyCallback(g_win, on_key);
    return 1;
}

void isaac_native_window_swap(void) {
    glfwSwapBuffers(g_win);
    glfwPollEvents();
    if (glfwWindowShouldClose(g_win)) isaac_native_request_quit();
}

void isaac_native_window_close(void) {
    if (!g_win) return;
    glfwDestroyWindow(g_win);
    glfwTerminate();
    g_win = NULL;
}

/* The first GLFW gamepad (SDL mapping, Xbox layout) as XINPUT_STATE. */
int isaac_native_pad_state(int slot, uint8_t *out) {
    GLFWgamepadstate st;
    if (slot != 0 || !g_win || !glfwJoystickIsGamepad(GLFW_JOYSTICK_1) ||
        !glfwGetGamepadState(GLFW_JOYSTICK_1, &st)) return 0;
    static const struct { int b; uint16_t bit; } BTN[] = {
        {GLFW_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {GLFW_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
        {GLFW_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004}, {GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {GLFW_GAMEPAD_BUTTON_START, 0x0010}, {GLFW_GAMEPAD_BUTTON_BACK, 0x0020},
        {GLFW_GAMEPAD_BUTTON_LEFT_THUMB, 0x0040}, {GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, 0x0080},
        {GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, 0x0100}, {GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, 0x0200},
        {GLFW_GAMEPAD_BUTTON_A, 0x1000}, {GLFW_GAMEPAD_BUTTON_B, 0x2000},
        {GLFW_GAMEPAD_BUTTON_X, 0x4000}, {GLFW_GAMEPAD_BUTTON_Y, 0x8000},
    };
    uint16_t w = 0;
    for (size_t i = 0; i < sizeof BTN / sizeof BTN[0]; ++i)
        if (st.buttons[BTN[i].b]) w |= BTN[i].bit;
    static uint32_t packet;
    ++packet;
    memcpy(out + 0, &packet, 4);
    memcpy(out + 4, &w, 2);
    out[6] = (uint8_t)((st.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 127.5f);
    out[7] = (uint8_t)((st.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 127.5f);
    int16_t s[4] = {   /* GLFW y grows downwards, XInput's upwards */
        (int16_t)(st.axes[GLFW_GAMEPAD_AXIS_LEFT_X] * 32767.0f),
        (int16_t)(-st.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] * 32767.0f),
        (int16_t)(st.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] * 32767.0f),
        (int16_t)(-st.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] * 32767.0f),
    };
    memcpy(out + 8, s, 8);
    return 1;
}
#endif
