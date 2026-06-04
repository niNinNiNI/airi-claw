---
{
  "name": "cap_asr",
  "description": "Record microphone audio and convert speech to text via cloud ASR. Use when you need to listen to the user's voice input, capture speech, and obtain the transcribed text.",
  "metadata": {
    "cap_groups": ["cap_asr"],
    "manage_mode": "readonly"
  }
}
---

## When to use this skill

Use `start_asr` when:
- You need to listen to what the user is saying through the microphone
- User explicitly asks you to listen or recognize speech
- You want to accept voice commands or voice input
- Building a voice-based interaction flow (listen → process → respond with TTS)

Do NOT use when:
- The device has no microphone (ADC not available)
- You only need to output speech (use `get_tts` from cap_voice instead)

## How to use

Call `start_asr` with optional parameters:

```json
{
  "max_duration_ms": 10000,
  "silence_timeout_ms": 1000
}
```

### Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `max_duration_ms` | integer | No | 10000 | Maximum recording duration in milliseconds (max 30000) |
| `silence_timeout_ms` | integer | No | 1000 | Auto-stop after this many milliseconds of silence (0 to disable) |

### Return value

On success, returns the recognized text as a string.

On failure, returns an error string starting with "Error:":
- `"Error: voice server not connected"` — Wi-Fi or server issue
- `"Error: no audio ADC device available"` — Hardware not ready
- `"Error: ASR result timeout"` — Cloud service did not respond
- `"Error: WebSocket disconnected"` — Connection lost during recording
- `"Error: <server message>"` — Server-side error
- `"Error: no result"` — No speech detected

## Important notes

1. **Mutual exclusion with TTS**: ASR recording and TTS playback cannot happen simultaneously. Wait for TTS to finish before starting ASR, and vice versa.
2. **Silence detection**: Recording stops automatically after `silence_timeout_ms` of silence. Speak clearly and avoid long pauses.
3. **Environment**: Works best in quiet environments. Background noise may interfere with recognition accuracy.
4. **Network**: Requires the voice server (WebSocket) to be connected and accessible.
5. **Timeout**: Total operation may take up to `max_duration_ms + 15 seconds` (for cloud ASR processing).
