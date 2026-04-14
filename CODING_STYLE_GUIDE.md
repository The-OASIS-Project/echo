# C Coding Style Guide
## ECHO Code Style Standards

This guide follows the same conventions as the DAWN project (and all OASIS daemons). Code should be readable, maintainable, and consistent across the ecosystem.

---

## 1. Indentation & Spacing

### Indentation
- **Use 3 spaces** for indentation (no tabs)

```c
void example_function(void) {
   if (condition) {
      do_something();
   }
}
```

### Spacing Around Operators
- Space around binary operators: `a + b`, `x == y`, `ptr->field`
- No space for unary operators: `!flag`, `*ptr`, `&variable`
- Space after commas: `function(a, b, c)`
- Space after keywords: `if (`, `for (`, `while (`
- Two spaces before trailing comments

### Pointer Alignment
- **Pointers align right** (to the variable name): `int *ptr`, `const char *str`
- Enforced by clang-format

### Line Length
- **100 characters maximum** (enforced by clang-format)

---

## 2. Braces & Control Structures

### Brace Style (K&R)
- Opening brace on same line
- **Always use braces** for single-statement blocks
- No single-line control structures

```c
if (condition) {
   single_statement();
} else {
   other_statement();
}
```

### Switch Statements
- Indent case labels one level
- Always include `break` or `/* fall through */`

---

## 3. Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Functions | `snake_case` | `modem_poll_signal()` |
| Variables | `snake_case` | `signal_dbm` |
| Constants/Macros | `UPPER_CASE` | `AT_TIMEOUT_DEFAULT` |
| Types | `_t` suffix | `call_state_t` |
| Header guards | `FILENAME_H` | `#ifndef AT_COMMAND_H` |

---

## 4. Comments & Documentation

### File Headers
All source files require the GPL license block:

```c
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Brief description of file purpose.
 */
```

### Function Documentation
- Doxygen-style for public APIs in headers
- Comment the "why" not the "what"

```c
/**
 * @brief Validate a phone number against the allowed pattern.
 * @param number  Null-terminated phone number string.
 * @return true if valid, false if empty, too long, or contains bad chars.
 */
bool sms_validate_number(const char *number);
```

---

## 5. Error Handling

- Return 0 on success, -1 or positive error codes on failure
- Always check return values from functions that can fail
- Log errors with `OLOG_ERROR()`

```c
at_status_t rc = at_command_send(ctx, "AT+CSQ", &resp, AT_TIMEOUT_DEFAULT);
if (rc != AT_OK) {
   OLOG_WARNING("AT+CSQ failed: %s", at_status_str(rc));
   return -1;
}
```

---

## 6. Memory Management

- **Prefer static allocation** — ECHO is a long-running daemon
- Use stack buffers with fixed sizes when possible
- Minimize dynamic allocation (malloc/calloc)
- When dynamic allocation is needed: always check for NULL, free and NULL

```c
/* Preferred: static allocation */
char buffer[AT_RESPONSE_MAX];

/* When necessary */
char *buf = malloc(size);
if (!buf) {
   OLOG_ERROR("Allocation failed");
   return -1;
}
/* ... use buf ... */
free(buf);
buf = NULL;
```

---

## 7. Threading

- Document thread safety in comments
- Use `__atomic` builtins for simple shared state
- Use mutex + condvar for complex synchronization
- Never call blocking AT commands from the URC reader thread

```c
/* Thread-safe: uses __atomic builtins */
static call_state_t g_call_state;

static void set_call_state(call_state_t state) {
   __atomic_store_n(&g_call_state, state, __ATOMIC_RELEASE);
}
```

---

## 8. Function Design

- Soft target: < 50 lines (no hard limit)
- Inputs first, outputs last in parameters
- One .h file per .c file

---

## 9. Logging

```c
#include "logging.h"

OLOG_INFO("Modem initialized");
OLOG_WARNING("Signal weak: %d dBm", dbm);
OLOG_ERROR("Serial port open failed: %s", strerror(errno));
```

---

## 10. Automated Formatting

```bash
# Format all code (required before committing)
./format_code.sh

# Check without modifying (CI mode)
./format_code.sh --check

# Format only changed files (fast)
./format_code.sh --changed
```

clang-format-14 enforces: indentation, line length, brace style, pointer alignment, include sorting, spacing. It does NOT enforce: naming, error handling, memory management, comment quality.

---

**Philosophy**: Consistency within the OASIS ecosystem matters. When this guide conflicts with getting things done, update the guide.
