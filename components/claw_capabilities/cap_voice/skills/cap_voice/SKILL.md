---
{
  "name": "cap_voice",
  "description": "Convert text to speech (TTS) and play through the built-in speaker using a local WebSocket TTS server.",
  "metadata": {
    "cap_groups": [
      "cap_voice"
    ],
    "manage_mode": "readonly"
  }
}
---

# Voice TTS

Use this skill when the user wants the device to speak text aloud through the speaker.

## When to use
- The user asks the device to say something out loud.
- The user requests a voice response, spoken greeting, or audible feedback.
- The user asks for TTS (text-to-speech) functionality.
- A response would be more useful spoken than displayed as text.

## Available capability
- `get_tts`: Convert text to speech via local WebSocket TTS server and play through the device speaker.

## Calling rules
- Call `get_tts` directly with a JSON object containing the text to speak.
- Input must be a JSON object:

```json
{
  "text": "The text to speak aloud",
  "voice": "zh_female_qingxin"
}
```

- The `text` field is required.
- The `voice` field is optional. If omitted, the default voice `zh_female_qingxin` is used.

## Output shape
- On success: `Done: <text>` — the text has been spoken.
- Common error strings include:
  - `Error: voice server not configured`
  - `Error: WebSocket connection failed`
  - `Error: no audio output device available`
  - `Error: invalid JSON`
  - `Error: text is required`
  - `Error: TTS request timed out`
  - `Error: Opus decoder init failed`
  - `Error: failed to allocate playback buffer`
  - `Error: failed to start playback (...)`

## Recommended workflow
1. Determine if the user wants spoken output.
2. Prepare the text to speak (can be the same as the textual response).
3. Call `get_tts` with the text.
4. Optionally also provide a textual response for confirmation.

## Common failure causes
- Calling `get_tts` without a `text` field.
- TTS server not reachable (server IP/port not configured or server offline).
- No audio output device (speaker/DAC) available on the board.
- WebSocket connection failure or server error.

## Examples

Speak a greeting:

```json
{
  "text": "Hello, I am ESP-Claw. How can I help you today?"
}
```

Speak with a specific voice:

```json
{
  "text": "The temperature is 25 degrees Celsius",
  "voice": "zh_female_qingxin"
}
