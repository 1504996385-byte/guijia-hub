`python tests/event_memory/run_tests.py` compiles the actual production event
header. It checks the 512-byte event budget and copied remote-message,
network-greeting and chat payloads, including action plus text/status together.
The old 732-byte type failed the budget; the shared action/message payload is
488 bytes and saves 15,616 bytes across the unchanged 64-slot event queue.

These checks use host type layout and copies. The ARM build and startup size
diagnostic verify the target layout; live heap and chat remain hardware checks.
The actual ARM flags enable short enums: compiling both layouts yields
716→472 bytes and the board startup reports 472. Savings remain 15,616 bytes.
