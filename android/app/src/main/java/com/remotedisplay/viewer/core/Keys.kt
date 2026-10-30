package com.remotedisplay.viewer.core

// Windows hardware scancodes (set 1), which the host injects so its own keyboard layout applies.
// Characters are mapped for a US layout: the scancode of the key plus whether Shift is held.
object Keys {
    class Stroke(val scan: Int, val shift: Boolean = false, val extended: Boolean = false)

    const val SHIFT = 0x2A
    const val CTRL = 0x1D
    const val ALT = 0x38
    const val WIN = 0x5B  // extended
    const val ENTER = 0x1C
    const val BACKSPACE = 0x0E
    const val TAB = 0x0F
    const val ESC = 0x01
    const val SPACE = 0x39
    const val DELETE = 0x53  // extended
    const val UP = 0x48      // extended
    const val DOWN = 0x50    // extended
    const val LEFT = 0x4B    // extended
    const val RIGHT = 0x4D   // extended
    const val HOME = 0x47    // extended
    const val END = 0x4F     // extended
    const val PAGE_UP = 0x49 // extended
    const val PAGE_DOWN = 0x51 // extended

    private val letters = intArrayOf(
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, // a..m
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, // n..z
    )
    private val digits = intArrayOf(0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A)  // 0..9

    private val plain = mapOf(
        ' ' to SPACE, '-' to 0x0C, '=' to 0x0D, '[' to 0x1A, ']' to 0x1B, '\\' to 0x2B,
        ';' to 0x27, '\'' to 0x28, '`' to 0x29, ',' to 0x33, '.' to 0x34, '/' to 0x35,
        '\n' to ENTER, '\t' to TAB,
    )
    private val shifted = mapOf(
        '!' to 0x02, '@' to 0x03, '#' to 0x04, '$' to 0x05, '%' to 0x06, '^' to 0x07, '&' to 0x08,
        '*' to 0x09, '(' to 0x0A, ')' to 0x0B, '_' to 0x0C, '+' to 0x0D, '{' to 0x1A, '}' to 0x1B,
        '|' to 0x2B, ':' to 0x27, '"' to 0x28, '~' to 0x29, '<' to 0x33, '>' to 0x34, '?' to 0x35,
    )

    // The keystroke that types `c`, or null for characters a US keyboard does not have.
    fun forChar(c: Char): Stroke? = when (c) {
        in 'a'..'z' -> Stroke(letters[c - 'a'])
        in 'A'..'Z' -> Stroke(letters[c - 'A'], shift = true)
        in '0'..'9' -> Stroke(digits[c - '0'])
        else -> plain[c]?.let { Stroke(it) } ?: shifted[c]?.let { Stroke(it, shift = true) }
    }

    fun isExtended(scan: Int) = scan in setOf(WIN, DELETE, UP, DOWN, LEFT, RIGHT, HOME, END, PAGE_UP, PAGE_DOWN)

    // Key down + key up events for one stroke (with Shift around it when needed).
    fun events(s: Stroke): List<InputEvent> {
        val ext = s.extended || isExtended(s.scan)
        val list = mutableListOf<InputEvent>()
        if (s.shift) list += InputEvent.key(SHIFT, false, up = false)
        list += InputEvent.key(s.scan, ext, up = false)
        list += InputEvent.key(s.scan, ext, up = true)
        if (s.shift) list += InputEvent.key(SHIFT, false, up = true)
        return list
    }
}
