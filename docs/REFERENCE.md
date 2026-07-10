# REAPER MCP — Tool, Resource & Prompt Reference

> **Generated** 2026-07-10 from the live in-process registry (`docs/gen/dump_reference.cpp` linked against `reaper_mcp_hostcore`). This mirrors exactly what the server serves over `tools/list`, `resources/list`, and `prompts/list` — it is not hand-maintained. Regenerate after any surface change with `cmake --build build --target reference-doc`.


**Protocol:** MCP `2025-06-18` · **Surface:** 186 tools · 3 resources · 3 prompts.


Tools are grouped by capability **profile**. Clients may negotiate a bounded profile set at `initialize` (to stay under LLM tool-count caps); `Profile::Full` (the default) exposes every tool. The always-on `tools.enumerate` meta-tool is visible under any profile.

## Surface at a glance

| Profile | Tools | Scope |
| --- | --- | --- |
| **Core** (`core`) | 82 | Transport, project, tracks, markers, items, tempo, time selection, and the always-on `tools.enumerate` meta-tool. |
| **Spatial / immersive** (`spatial`) | 23 | Immersive/spatial audio: channel beds, ambisonic encode/decode, surround panners, scene rotation, binaural monitoring, and live head-tracking. |
| **Mixing** (`mixing`) | 27 | Track FX, parameter automation envelopes, faders, and immersive-aware style chains. |
| **Routing** (`routing`) | 8 | Track-to-track sends and channel-count management. |
| **MIDI** (`midi`) | 23 | Takes and MIDI note / CC CRUD. |
| **Render** (`render`) | 4 | Multichannel / immersive deliverable rendering. |
| **Analysis** (`analysis`) | 18 | Deliverable-spec conformance (loudness + true-peak, per-bed). |
| **Composite / DSL** (`full`) | 1 | The deterministic composite macro-DSL runner (`$ref`/capture, atomic single-undo). |
| **Total** | **186** | |

## Contents

- [Tools](#tools)
  - [Core](#core)
  - [Spatial / immersive](#spatial--immersive)
  - [Mixing](#mixing)
  - [Routing](#routing)
  - [MIDI](#midi)
  - [Render](#render)
  - [Analysis](#analysis)
  - [Composite / DSL](#composite--dsl)
- [Resources](#resources)
- [Prompts](#prompts)

## Tools

### Core

_Transport, project, tracks, markers, items, tempo, time selection, and the always-on `tools.enumerate` meta-tool._

#### `action.get_toggle_state`

**Profile:** `core` · **Hints:** read-only, idempotent

Read the toggle/on-off state of a main-section action by numeric command ID. toggleState: -1 = not a toggle / unavailable, 0 = off, 1 = on.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `command` | integer | yes | numeric main-section command ID |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `available`, `command`, `name`, `toggleState`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "command": {
        "description": "numeric main-section command ID",
        "type": "integer"
      }
    },
    "required": [
      "command"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "available": {
        "type": "boolean"
      },
      "command": {
        "type": "integer"
      },
      "name": {
        "type": "string"
      },
      "toggleState": {
        "type": "integer"
      }
    },
    "required": [
      "command",
      "toggleState"
    ],
    "type": "object"
  }
}
```
</details>

#### `action.run`

**Profile:** `core` · **Hints:** **destructive**

Run a REAPER action by its numeric main-section command ID (from the Actions window), wrapped in one undo block. Blocked for known modal/fatal actions (see actionsAllowList). Returns the action's post-run toggle state (-1 = not a toggle).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `command` | integer | yes | numeric main-section command ID |
| `dryRun` | boolean | no | resolve + policy-check only; run nothing |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `command`, `detail`, `dryRun`, `error`, `name`, `ok`, `reason`, `toggleState`, `wouldRun`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "command": {
        "description": "numeric main-section command ID",
        "type": "integer"
      },
      "dryRun": {
        "description": "resolve + policy-check only; run nothing",
        "type": "boolean"
      }
    },
    "required": [
      "command"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "command": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "name": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "reason": {
        "type": "string"
      },
      "toggleState": {
        "type": "integer"
      },
      "wouldRun": {
        "type": "boolean"
      }
    },
    "required": [
      "command"
    ],
    "type": "object"
  }
}
```
</details>

#### `action.run_by_name`

**Profile:** `core` · **Hints:** **destructive**

Resolve a NAMED REAPER command (e.g. an SWS action "_SWS_..." or a custom-action/ReaScript "_RS..." id) via NamedCommandLookup, then run it under the same undo + deny-list rails as action.run. For plain numeric IDs use action.run.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `dryRun` | boolean | no | resolve + policy-check only; run nothing |
| `name` | string | yes | named command id, e.g. _SWS_ABOUT |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `command`, `detail`, `dryRun`, `error`, `name`, `ok`, `reason`, `resolvedFrom`, `toggleState`, `wouldRun`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "dryRun": {
        "description": "resolve + policy-check only; run nothing",
        "type": "boolean"
      },
      "name": {
        "description": "named command id, e.g. _SWS_ABOUT",
        "type": "string"
      }
    },
    "required": [
      "name"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "command": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "name": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "reason": {
        "type": "string"
      },
      "resolvedFrom": {
        "type": "string"
      },
      "toggleState": {
        "type": "integer"
      },
      "wouldRun": {
        "type": "boolean"
      }
    },
    "required": [],
    "type": "object"
  }
}
```
</details>

#### `edit.get_undo_state`

**Profile:** `core` · **Hints:** read-only, idempotent

Report whether undo/redo are available and the name of each next step.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `canRedo`, `canUndo`, `redoName`, `undoName`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "canRedo": {
        "type": "boolean"
      },
      "canUndo": {
        "type": "boolean"
      },
      "redoName": {
        "type": "string"
      },
      "undoName": {
        "type": "string"
      }
    },
    "required": [
      "canUndo",
      "canRedo"
    ],
    "type": "object"
  }
}
```
</details>

#### `edit.redo`

**Profile:** `core` · **Hints:** mutating

Redo the most recently undone action (REAPER's global undo stack).

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `ok`, `redone`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "redone": {
        "type": "string"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `edit.undo`

**Profile:** `core` · **Hints:** mutating

Undo the most recent undoable action (REAPER's global undo stack).

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `ok`, `undone`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "undone": {
        "type": "string"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.add`

**Profile:** `core` · **Hints:** mutating

Add an empty media item to a track at a position (default 0) and length (default 1s). Foundation for takes / MIDI / audio inserts.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `length` | number | no | min 0 |
| `position` | number | no | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `guid`, `length`, `position`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "length": {
        "minimum": 0,
        "type": "number"
      },
      "position": {
        "minimum": 0,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "guid": {
        "type": "string"
      },
      "length": {
        "type": "number"
      },
      "position": {
        "type": "number"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "position",
      "length"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.delete`

**Profile:** `core` · **Hints:** **destructive**

Delete a media item from its track. Destructive; single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `item`, `ok`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "item": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.duplicate`

**Profile:** `core` · **Hints:** mutating

Duplicate a media item in place. Wraps REAPER action 41295 on just this item.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.glue`

**Profile:** `core` · **Hints:** mutating

Glue a media item (render its takes/edits to a single new item), ignoring the time selection. Wraps REAPER action 41588 on just this item.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.list`

**Profile:** `core` · **Hints:** read-only, idempotent

List a track's media items with index, position, length, GUID.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `itemCount`, `items`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "itemCount": {
        "type": "integer"
      },
      "items": {
        "items": {
          "properties": {
            "guid": {
              "type": "string"
            },
            "index": {
              "type": "integer"
            },
            "length": {
              "type": "number"
            },
            "position": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "itemCount",
      "items"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.move`

**Profile:** `core` · **Hints:** idempotent

Move a media item to a new position and/or another track. Provide position (seconds) and/or destTrack (index). Idempotent.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `destTrack` | integer | no | min 0 |
| `item` | integer | yes | min 0 |
| `position` | number | no | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `movedTrack`, `ok`, `position`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "destTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "position": {
        "minimum": 0,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "movedTrack": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      },
      "position": {
        "type": "number"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.set_color`

**Profile:** `core` · **Hints:** idempotent

Set a media item's custom color (0xRRGGBB); color:0 clears it.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `color` | integer | yes | min 0; 0xRRGGBB; 0 = clear |
| `item` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `color`, `colorSet`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "color": {
        "description": "0xRRGGBB; 0 = clear",
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "color"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "color": {
        "type": "integer"
      },
      "colorSet": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "color",
      "colorSet"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.set_fade_in`

**Profile:** `core` · **Hints:** idempotent

Set a media item's fade-in length (seconds; D_FADEINLEN).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `length` | number | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `length`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "length": {
        "minimum": 0,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "length"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "length": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "length"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.set_fade_out`

**Profile:** `core` · **Hints:** idempotent

Set a media item's fade-out length (seconds; D_FADEOUTLEN).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `length` | number | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `length`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "length": {
        "minimum": 0,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "length"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "length": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "length"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.set_length`

**Profile:** `core` · **Hints:** idempotent

Set a media item's length (seconds).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `length` | number | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `length`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "length": {
        "minimum": 0,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "length"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "length": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "length"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.set_mute`

**Profile:** `core` · **Hints:** idempotent

Mute or unmute a media item (B_MUTE).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `muted` | boolean | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `muted`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "muted": {
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "muted"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "muted": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "muted"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.set_position`

**Profile:** `core` · **Hints:** idempotent

Set a media item's start position (seconds).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `position` | number | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `position`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "position": {
        "minimum": 0,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "position"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "position": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "position"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.set_selected`

**Profile:** `core` · **Hints:** idempotent

Select or deselect a single media item.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `selected` | boolean | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `selected`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "selected": {
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "selected"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "selected": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "selected"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.set_vol`

**Profile:** `core` · **Hints:** idempotent

Set a media item's take/item volume in dB (D_VOL).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `db` | number | yes | — |
| `item` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `db`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "db": {
        "type": "number"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "db"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "db": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "db"
    ],
    "type": "object"
  }
}
```
</details>

#### `item.split`

**Profile:** `core` · **Hints:** mutating

Split a media item at an absolute time (seconds). Returns the GUIDs of the left (original) and right (new) halves; ok=false if the time is outside the item.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `position` | number | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `leftGuid`, `ok`, `rightGuid`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "position": {
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "position"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "leftGuid": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "rightGuid": {
        "type": "string"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `items.implode_to_takes`

**Profile:** `core` · **Hints:** mutating

Implode media items into a single multi-take item for comping. Selects the given items (array of {track,item}) — or uses the current item selection if 'items' is omitted — then wraps REAPER action 40438 (Implode items across tracks into takes). Returns the resulting take count of the imploded item.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `items` | array&lt;object&gt; | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `selectedItems`, `takeCount`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "items": {
        "items": {
          "additionalProperties": false,
          "properties": {
            "item": {
              "minimum": 0,
              "type": "integer"
            },
            "track": {
              "minimum": 0,
              "type": "integer"
            }
          },
          "required": [
            "track",
            "item"
          ],
          "type": "object"
        },
        "minItems": 1,
        "type": "array"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "selectedItems": {
        "type": "integer"
      },
      "takeCount": {
        "type": "integer"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `marker.add`

**Profile:** `core` · **Hints:** mutating

Add a project marker at a position (seconds), optionally named and colored.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `color` | integer | no | 0xRRGGBB; 0 = default |
| `name` | string | no | — |
| `position` | number | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `index`, `position`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "color": {
        "description": "0xRRGGBB; 0 = default",
        "type": "integer"
      },
      "name": {
        "type": "string"
      },
      "position": {
        "type": "number"
      }
    },
    "required": [
      "position"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "index": {
        "type": "integer"
      },
      "position": {
        "type": "number"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `marker.delete`

**Profile:** `core` · **Hints:** **destructive**

Delete a marker or region by its REAPER index number (from markers.list).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `index` | integer | yes | — |
| `isRegion` | boolean | no | default `false` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `index`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "index": {
        "type": "integer"
      },
      "isRegion": {
        "default": false,
        "type": "boolean"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "index": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `marker.edit`

**Profile:** `core` · **Hints:** idempotent

Edit an existing marker or region by index number: any of name, position, end (regions), color. Unspecified fields are preserved.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `color` | integer | no | 0xRRGGBB; 0 = default |
| `end` | number | no | — |
| `index` | integer | yes | — |
| `isRegion` | boolean | no | default `false` |
| `name` | string | no | — |
| `position` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `end`, `index`, `name`, `ok`, `position`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "color": {
        "description": "0xRRGGBB; 0 = default",
        "type": "integer"
      },
      "end": {
        "type": "number"
      },
      "index": {
        "type": "integer"
      },
      "isRegion": {
        "default": false,
        "type": "boolean"
      },
      "name": {
        "type": "string"
      },
      "position": {
        "type": "number"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "end": {
        "type": "number"
      },
      "index": {
        "type": "integer"
      },
      "name": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "position": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `marker.goto`

**Profile:** `core` · **Hints:** idempotent

Move the edit cursor to a marker by its index number (from markers.list).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `index` | integer | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `position`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "index": {
        "type": "integer"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "position": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `markers.list`

**Profile:** `core` · **Hints:** read-only, idempotent

List all project markers and regions (index number, name, position, color).

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `count`, `markers`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "count": {
        "type": "integer"
      },
      "markers": {
        "items": {
          "properties": {
            "color": {
              "type": "integer"
            },
            "end": {
              "type": "number"
            },
            "index": {
              "type": "integer"
            },
            "isRegion": {
              "type": "boolean"
            },
            "name": {
              "type": "string"
            },
            "position": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      }
    },
    "required": [
      "count",
      "markers"
    ],
    "type": "object"
  }
}
```
</details>

#### `project.get_summary`

**Profile:** `core` · **Hints:** read-only, idempotent

Summarize the project: name, sample rate, track count, length.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `lengthSec`, `name`, `sampleRate`, `trackCount`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "lengthSec": {
        "type": "number"
      },
      "name": {
        "type": "string"
      },
      "sampleRate": {
        "type": "integer"
      },
      "trackCount": {
        "type": "integer"
      }
    },
    "required": [
      "name",
      "trackCount",
      "lengthSec"
    ],
    "type": "object"
  }
}
```
</details>

#### `project.new`

**Profile:** `core` · **Hints:** **destructive**

Start a new empty project. Refuses with 'unsaved_changes' if the current project is dirty (a new-project action on a dirty project would open a blocking Save prompt); save or open a project first. Never shows a blocking dialog.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `detail`, `error`, `ok`, `remediation`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "detail": {
        "type": "string"
      },
      "error": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "remediation": {
        "type": "string"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `project.open`

**Profile:** `core` · **Hints:** **destructive**

Open a project file by absolute path. Refuses with 'unsaved_changes' if the current project is dirty, unless 'discardChanges':true (which opens via REAPER's no-prompt path, discarding the current project's unsaved edits). Never shows a blocking dialog.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `discardChanges` | boolean | no | default `false` |
| `path` | string | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `detail`, `error`, `ok`, `path`, `remediation`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "discardChanges": {
        "default": false,
        "type": "boolean"
      },
      "path": {
        "type": "string"
      }
    },
    "required": [
      "path"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "detail": {
        "type": "string"
      },
      "error": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "path": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `project.save`

**Profile:** `core` · **Hints:** idempotent

Save the current project. With 'path', saves-as to that .rpp path (no dialog) and makes it the project file. Without 'path', saves to the existing file; if the project has never been saved, returns a 'no_project_path' error rather than opening a blocking Save-As dialog.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `path` | string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `detail`, `error`, `ok`, `path`, `remediation`, `savedAs`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "path": {
        "type": "string"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "detail": {
        "type": "string"
      },
      "error": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "path": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "savedAs": {
        "type": "boolean"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `region.add`

**Profile:** `core` · **Hints:** mutating

Add a project region spanning [start,end) seconds, optionally named/colored.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `color` | integer | no | 0xRRGGBB; 0 = default |
| `end` | number | yes | — |
| `name` | string | no | — |
| `start` | number | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `end`, `index`, `start`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "color": {
        "description": "0xRRGGBB; 0 = default",
        "type": "integer"
      },
      "end": {
        "type": "number"
      },
      "name": {
        "type": "string"
      },
      "start": {
        "type": "number"
      }
    },
    "required": [
      "start",
      "end"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "end": {
        "type": "number"
      },
      "index": {
        "type": "integer"
      },
      "start": {
        "type": "number"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `region.delete`

**Profile:** `core` · **Hints:** **destructive**

Delete a region by its REAPER index number (from region.list).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `index` | integer | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `index`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "index": {
        "type": "integer"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "index": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `region.goto`

**Profile:** `core` · **Hints:** idempotent

Move the edit cursor to a region's start by its index number (from region.list).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `index` | integer | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `position`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "index": {
        "type": "integer"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "position": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `region.list`

**Profile:** `core` · **Hints:** read-only, idempotent

List only project regions (index number, name, start, end, color).

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `count`, `regions`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "count": {
        "type": "integer"
      },
      "regions": {
        "items": {
          "properties": {
            "color": {
              "type": "integer"
            },
            "end": {
              "type": "number"
            },
            "index": {
              "type": "integer"
            },
            "name": {
              "type": "string"
            },
            "start": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      }
    },
    "required": [
      "count",
      "regions"
    ],
    "type": "object"
  }
}
```
</details>

#### `selection.get`

**Profile:** `core` · **Hints:** read-only, idempotent

Return the currently selected tracks (indices) and media items (track+item indices, GUID).

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `itemCount`, `items`, `trackCount`, `tracks`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "itemCount": {
        "type": "integer"
      },
      "items": {
        "items": {
          "properties": {
            "guid": {
              "type": "string"
            },
            "item": {
              "type": "integer"
            },
            "track": {
              "type": "integer"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "trackCount": {
        "type": "integer"
      },
      "tracks": {
        "items": {
          "type": "integer"
        },
        "type": "array"
      }
    },
    "required": [
      "trackCount",
      "itemCount",
      "tracks",
      "items"
    ],
    "type": "object"
  }
}
```
</details>

#### `selection.set`

**Profile:** `core` · **Hints:** idempotent

Set the track and/or item selection. deselectAll clears both axes first; providing tracks or items replaces that axis with exactly the given set. items are {track,item} index pairs.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `deselectAll` | boolean | no | default `false` |
| `items` | array&lt;object&gt; | no | — |
| `tracks` | array&lt;integer&gt; | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `selectedItems`, `selectedTracks`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "deselectAll": {
        "default": false,
        "type": "boolean"
      },
      "items": {
        "items": {
          "properties": {
            "item": {
              "minimum": 0,
              "type": "integer"
            },
            "track": {
              "minimum": 0,
              "type": "integer"
            }
          },
          "required": [
            "track",
            "item"
          ],
          "type": "object"
        },
        "type": "array"
      },
      "tracks": {
        "items": {
          "minimum": 0,
          "type": "integer"
        },
        "type": "array"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "selectedItems": {
        "type": "integer"
      },
      "selectedTracks": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "selectedTracks",
      "selectedItems"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.add`

**Profile:** `core` · **Hints:** mutating

Append a new empty take to a media item (foundation for comping / layering). Returns the new take index and the item's take count.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `makeActive` | boolean | no | default `false` |
| `name` | string | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `take`, `takeCount`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "makeActive": {
        "default": false,
        "type": "boolean"
      },
      "name": {
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "take": {
        "type": "integer"
      },
      "takeCount": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "take",
      "takeCount"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.crop_to_active`

**Profile:** `core` · **Hints:** **destructive**

Crop a media item to its active take, discarding all other takes. Wraps REAPER action 40131 on just this item. Destructive; single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `takeCount`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "takeCount": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "takeCount"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.delete`

**Profile:** `core` · **Hints:** **destructive**

Delete a take from a media item by index (default: the active take). Wraps REAPER action 40129 on just this item. Refuses to delete the only take (that would delete the item — use item.delete). Destructive; single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `takeCount`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "takeCount": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "takeCount"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.set_pan`

**Profile:** `core` · **Hints:** idempotent

Set a take's pan (-1 left .. +1 right; default: the active take; D_PAN).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `pan` | number | yes | range [-1, 1] |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `pan`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "pan": {
        "maximum": 1,
        "minimum": -1,
        "type": "number"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "pan"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "pan": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "pan"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.set_pitch`

**Profile:** `core` · **Hints:** idempotent

Set a take's pitch adjustment in semitones (default: the active take; D_PITCH). preservePitch toggles time-stretch pitch preservation (B_PPITCH).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `preservePitch` | boolean | no | — |
| `semitones` | number | yes | — |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `semitones`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "preservePitch": {
        "type": "boolean"
      },
      "semitones": {
        "type": "number"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "semitones"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "semitones": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "semitones"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.set_playrate`

**Profile:** `core` · **Hints:** idempotent

Set a take's playback rate (default: the active take; D_PLAYRATE; >0). preservePitch toggles pitch preservation while stretching (B_PPITCH).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `preservePitch` | boolean | no | — |
| `rate` | number | yes | — |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `rate`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "preservePitch": {
        "type": "boolean"
      },
      "rate": {
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "rate"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "rate": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "rate"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.set_vol`

**Profile:** `core` · **Hints:** idempotent

Set a take's volume in dB (default: the active take; D_VOL).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `db` | number | yes | — |
| `item` | integer | yes | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `db`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "db": {
        "type": "number"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "db"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "db": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "db"
    ],
    "type": "object"
  }
}
```
</details>

#### `tempo.add_marker`

**Profile:** `core` · **Hints:** mutating

Add a tempo/time-signature marker at a time position (seconds). 'bpm' sets the tempo from this point; optional 'timesigNum'/'timesigDenom' change the time signature (omit or 0 to keep the previous one); 'linear' makes the tempo ramp linearly to the next marker. Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bpm` | number | yes | range [1, 960] |
| `linear` | boolean | no | default `false` |
| `position` | number | yes | min 0 |
| `timesigDenom` | integer | no | min 0 |
| `timesigNum` | integer | no | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `count`, `index`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bpm": {
        "maximum": 960,
        "minimum": 1,
        "type": "number"
      },
      "linear": {
        "default": false,
        "type": "boolean"
      },
      "position": {
        "minimum": 0,
        "type": "number"
      },
      "timesigDenom": {
        "minimum": 0,
        "type": "integer"
      },
      "timesigNum": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "position",
      "bpm"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "count": {
        "type": "integer"
      },
      "index": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `tempo.delete_marker`

**Profile:** `core` · **Hints:** **destructive**

Delete a tempo/time-signature marker by index (from tempo.list_markers). Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `index` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `count`, `ok`, `removedIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "index": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "count": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "removedIndex": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "removedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `tempo.edit_marker`

**Profile:** `core` · **Hints:** idempotent

Edit an existing tempo/time-signature marker by index (from tempo.list_markers). Only the fields you provide change; the rest keep their current values (position, bpm, timesigNum, timesigDenom, linear). Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bpm` | number | no | range [1, 960] |
| `index` | integer | yes | min 0 |
| `linear` | boolean | no | — |
| `position` | number | no | min 0 |
| `timesigDenom` | integer | no | min 0 |
| `timesigNum` | integer | no | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `bpm`, `index`, `ok`, `position`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bpm": {
        "maximum": 960,
        "minimum": 1,
        "type": "number"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "linear": {
        "type": "boolean"
      },
      "position": {
        "minimum": 0,
        "type": "number"
      },
      "timesigDenom": {
        "minimum": 0,
        "type": "integer"
      },
      "timesigNum": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "bpm": {
        "type": "number"
      },
      "index": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "position": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `tempo.get`

**Profile:** `core` · **Hints:** read-only, idempotent

Get the project's current/master tempo in BPM.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `bpm`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "bpm": {
        "type": "number"
      }
    },
    "required": [
      "bpm"
    ],
    "type": "object"
  }
}
```
</details>

#### `tempo.list_markers`

**Profile:** `core` · **Hints:** read-only, idempotent

List all tempo/time-signature markers: index, time (sec), measure/beat, BPM, time signature, and whether the tempo transition is linear.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `count`, `markers`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "count": {
        "type": "integer"
      },
      "markers": {
        "items": {
          "properties": {
            "beat": {
              "type": "number"
            },
            "bpm": {
              "type": "number"
            },
            "index": {
              "type": "integer"
            },
            "linear": {
              "type": "boolean"
            },
            "measure": {
              "type": "integer"
            },
            "position": {
              "type": "number"
            },
            "timesigDenom": {
              "type": "integer"
            },
            "timesigNum": {
              "type": "integer"
            }
          },
          "type": "object"
        },
        "type": "array"
      }
    },
    "required": [
      "count",
      "markers"
    ],
    "type": "object"
  }
}
```
</details>

#### `tempo.set`

**Profile:** `core` · **Hints:** idempotent

Set the project tempo in BPM.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bpm` | number | yes | range [1, 960] |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `bpm`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bpm": {
        "maximum": 960,
        "minimum": 1,
        "type": "number"
      }
    },
    "required": [
      "bpm"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "bpm": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "bpm"
    ],
    "type": "object"
  }
}
```
</details>

#### `time.qn_to_time`

**Profile:** `core` · **Hints:** read-only, idempotent

Convert a project quarter-note position (QN, counted from project start) to a time position in seconds, honoring the full tempo map.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `qn` | number | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `qn`, `time`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "qn": {
        "type": "number"
      }
    },
    "required": [
      "qn"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "qn": {
        "type": "number"
      },
      "time": {
        "type": "number"
      }
    },
    "required": [
      "qn",
      "time"
    ],
    "type": "object"
  }
}
```
</details>

#### `time.time_to_qn`

**Profile:** `core` · **Hints:** read-only, idempotent

Convert a time position in seconds to a project quarter-note position (QN, counted from project start), honoring the full tempo map.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `time` | number | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `qn`, `time`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "time": {
        "type": "number"
      }
    },
    "required": [
      "time"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "qn": {
        "type": "number"
      },
      "time": {
        "type": "number"
      }
    },
    "required": [
      "time",
      "qn"
    ],
    "type": "object"
  }
}
```
</details>

#### `timeselection.get`

**Profile:** `core` · **Hints:** read-only, idempotent

Get the time selection, the loop range, and whether looping is enabled.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `loopEnabled`, `loopEnd`, `loopStart`, `timeSelEnd`, `timeSelStart`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "loopEnabled": {
        "type": "boolean"
      },
      "loopEnd": {
        "type": "number"
      },
      "loopStart": {
        "type": "number"
      },
      "timeSelEnd": {
        "type": "number"
      },
      "timeSelStart": {
        "type": "number"
      }
    },
    "required": [
      "timeSelStart",
      "timeSelEnd",
      "loopEnabled"
    ],
    "type": "object"
  }
}
```
</details>

#### `timeselection.set`

**Profile:** `core` · **Hints:** idempotent

Set the time selection (default) or the loop range to [start,end) seconds.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `end` | number | yes | — |
| `start` | number | yes | — |
| `target` | enum | no | one of: `timesel`, `loop`; default `"timesel"` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `end`, `ok`, `start`, `target`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "end": {
        "type": "number"
      },
      "start": {
        "type": "number"
      },
      "target": {
        "default": "timesel",
        "enum": [
          "timesel",
          "loop"
        ],
        "type": "string"
      }
    },
    "required": [
      "start",
      "end"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "end": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      },
      "start": {
        "type": "number"
      },
      "target": {
        "type": "string"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `tools.enumerate`

**Profile:** `core` · **Hints:** read-only, idempotent · **always visible**

Meta-tool: list every available tool, INCLUDING tools outside the session's negotiated profile set, optionally filtered by profile or a name/description substring. Any tool listed here can be invoked by name via tools/call even if it did not appear in tools/list. Use this to stay under a client's tool-count cap: negotiate a small profile at initialize, then discover and call the rest on demand. Pass detail=true to also get each tool's JSON schemas.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `detail` | boolean | no | default `false`; include each tool's full input/output JSON schemas |
| `profile` | enum | no | one of: `core`, `midi`, `mixing`, `routing`, `spatial`, `render`, `analysis`, `full`, `all`; restrict to one capability profile; 'all'/'full' = no filter |
| `query` | string | no | case-insensitive substring matched against tool name + description |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `count`, `tools`, `totalRegistered`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "detail": {
        "default": false,
        "description": "include each tool's full input/output JSON schemas",
        "type": "boolean"
      },
      "profile": {
        "description": "restrict to one capability profile; 'all'/'full' = no filter",
        "enum": [
          "core",
          "midi",
          "mixing",
          "routing",
          "spatial",
          "render",
          "analysis",
          "full",
          "all"
        ],
        "type": "string"
      },
      "query": {
        "description": "case-insensitive substring matched against tool name + description",
        "type": "string"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "count": {
        "type": "integer"
      },
      "tools": {
        "items": {
          "properties": {
            "description": {
              "type": "string"
            },
            "destructive": {
              "type": "boolean"
            },
            "idempotent": {
              "type": "boolean"
            },
            "name": {
              "type": "string"
            },
            "profile": {
              "type": "string"
            },
            "readOnly": {
              "type": "boolean"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "totalRegistered": {
        "type": "integer"
      }
    },
    "required": [
      "count",
      "totalRegistered",
      "tools"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.add`

**Profile:** `core` · **Hints:** mutating

Insert a new track at an index (default: end) and optionally name it.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `index` | integer | no | min 0 |
| `name` | string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `guid`, `trackIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "name": {
        "type": "string"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "guid": {
        "type": "string"
      },
      "trackIndex": {
        "type": "integer"
      }
    },
    "required": [
      "trackIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.freeze`

**Profile:** `core` · **Hints:** **destructive**

Freeze a track (render items+FX to audio, take FX offline): mode 'stereo' (default), 'mono', or 'multichannel' (use multichannel to preserve bed/ambisonic widths). Blocks until the render completes; reversible with track.unfreeze.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `mode` | enum | no | one of: `stereo`, `mono`, `multichannel`; default `"stereo"` |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxCount`, `itemCount`, `mode`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "mode": {
        "default": "stereo",
        "enum": [
          "stereo",
          "mono",
          "multichannel"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxCount": {
        "type": "integer"
      },
      "itemCount": {
        "type": "integer"
      },
      "mode": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "mode"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.get`

**Profile:** `core` · **Hints:** read-only, idempotent

Get one track's full state: name, GUID, volume (dB), pan, width, channels, mute/solo/phase, record arm/input/monitor/mode, automation mode, folder depth/compact, color.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `automationMode`, `channels`, `color`, `colorSet`, `folderCompact`, `folderDepth`, `guid`, `index`, `muted`, `name`, `pan`, `phaseInverted`, `recArmed`, `recInput`, `recMode`, `recMon`, `soloed`, `volDb`, `width`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "automationMode": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "color": {
        "type": "integer"
      },
      "colorSet": {
        "type": "boolean"
      },
      "folderCompact": {
        "type": "integer"
      },
      "folderDepth": {
        "type": "integer"
      },
      "guid": {
        "type": "string"
      },
      "index": {
        "type": "integer"
      },
      "muted": {
        "type": "boolean"
      },
      "name": {
        "type": "string"
      },
      "pan": {
        "type": "number"
      },
      "phaseInverted": {
        "type": "boolean"
      },
      "recArmed": {
        "type": "boolean"
      },
      "recInput": {
        "type": "integer"
      },
      "recMode": {
        "type": "integer"
      },
      "recMon": {
        "type": "integer"
      },
      "soloed": {
        "type": "boolean"
      },
      "volDb": {
        "type": "number"
      },
      "width": {
        "type": "number"
      }
    },
    "required": [
      "index",
      "name",
      "volDb",
      "pan",
      "channels",
      "muted",
      "soloed"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.get_name`

**Profile:** `core` · **Hints:** read-only, idempotent

Get a track's name by index.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `name`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "name": {
        "type": "string"
      }
    },
    "required": [
      "name"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.get_peak`

**Profile:** `core` · **Hints:** read-only, idempotent

Read a track's live peak meter per channel (linear, 1.0 = 0 dBFS, plus dB). Meaningful while audio is playing/monitoring; silent tracks report -inf (-150 dB floor). Pass 'channel' for one channel, else all of the track's channels are returned.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `channel` | integer | no | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `channels`, `peaks`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "channel": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channels": {
        "type": "integer"
      },
      "peaks": {
        "items": {
          "properties": {
            "channel": {
              "type": "integer"
            },
            "peak": {
              "type": "number"
            },
            "peakDb": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "channels",
      "peaks"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.list`

**Profile:** `core` · **Hints:** read-only, idempotent

List all tracks with index, name, GUID, volume (dB), pan, channels, mute/solo.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `trackCount`, `tracks`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "trackCount": {
        "type": "integer"
      },
      "tracks": {
        "items": {
          "properties": {
            "channels": {
              "type": "integer"
            },
            "guid": {
              "type": "string"
            },
            "index": {
              "type": "integer"
            },
            "muted": {
              "type": "boolean"
            },
            "name": {
              "type": "string"
            },
            "pan": {
              "type": "number"
            },
            "soloed": {
              "type": "boolean"
            },
            "volDb": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      }
    },
    "required": [
      "trackCount",
      "tracks"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.patch_state`

**Profile:** `core` · **Hints:** **destructive**

Patch specific TOP-LEVEL keys of a track's state chunk in place (e.g. NAME, PEAKCOL, VOLPAN, MUTESOLO, REC) — a narrower, safer alternative to track.set_state_chunk's wholesale replace. Reads the current <TRACK ...> chunk, rewrites each named key's line, and applies via the same validated write path (structural validation, dry-run unified diff, single undo block, elicitation/confirm gate). Only EXISTING top-level keys are patched; a key inside a nested block (FX chain, items, envelopes) is not reachable here (use track.set_state_chunk). An unchanged patch is a no-op.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `confirm` | boolean | no | default `false` |
| `context` | integer | no | default `3`; min 0 |
| `dryRun` | boolean | no | default `false` |
| `patches` | array&lt;object&gt; | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `addedLines`, `applied`, `changed`, `detail`, `diff`, `dryRun`, `error`, `keys`, `note`, `ok`, `remediation`, `removedLines`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "confirm": {
        "default": false,
        "type": "boolean"
      },
      "context": {
        "default": 3,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "patches": {
        "items": {
          "additionalProperties": false,
          "properties": {
            "key": {
              "type": "string"
            },
            "value": {
              "type": "string"
            }
          },
          "required": [
            "key",
            "value"
          ],
          "type": "object"
        },
        "minItems": 1,
        "type": "array"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "patches"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "addedLines": {
        "type": "integer"
      },
      "applied": {
        "type": "boolean"
      },
      "changed": {
        "type": "boolean"
      },
      "detail": {
        "type": "string"
      },
      "diff": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "keys": {
        "items": {
          "type": "string"
        },
        "type": "array"
      },
      "note": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "remediation": {
        "type": "string"
      },
      "removedLines": {
        "type": "integer"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "track"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.remove`

**Profile:** `core` · **Hints:** **destructive**

Delete a track by index. Destructive; single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `removedIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "removedIndex": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "removedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.select`

**Profile:** `core` · **Hints:** idempotent

Select a track; exclusive=true (default) clears other selections.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `exclusive` | boolean | no | default `true` |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "exclusive": {
        "default": true,
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_color`

**Profile:** `core` · **Hints:** idempotent

Set a track's custom color (0xRRGGBB); color:0 clears the custom color.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `color` | integer | yes | min 0; 0xRRGGBB; 0 = clear |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `color`, `colorSet`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "color": {
        "description": "0xRRGGBB; 0 = clear",
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "color"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "color": {
        "type": "integer"
      },
      "colorSet": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "color",
      "colorSet"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_folder`

**Profile:** `core` · **Hints:** idempotent

Set a track's folder depth (I_FOLDERDEPTH: 1 = folder parent, 0 = normal, negative = close N folders after this track) and optional folder-compact state (I_FOLDERCOMPACT: 0 open, 1 small, 2 closed).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `compact` | integer | no | range [0, 2] |
| `depth` | integer | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `compact`, `depth`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "compact": {
        "maximum": 2,
        "minimum": 0,
        "type": "integer"
      },
      "depth": {
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "depth"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "compact": {
        "type": "integer"
      },
      "depth": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "depth"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_mute`

**Profile:** `core` · **Hints:** idempotent

Mute or unmute a track (B_MUTE).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `muted` | boolean | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `muted`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "muted": {
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "muted"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "muted": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "muted"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_name`

**Profile:** `core` · **Hints:** idempotent

Rename a track by index.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `name` | string | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `name`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "name": {
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "name"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "name": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "name"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_phase`

**Profile:** `core` · **Hints:** idempotent

Invert (flip) a track's polarity/phase, or clear it (B_PHASE).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `inverted` | boolean | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `inverted`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "inverted": {
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "inverted"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "inverted": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "inverted"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_rec_arm`

**Profile:** `core` · **Hints:** idempotent

Arm or disarm a track for recording (I_RECARM).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `armed` | boolean | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `armed`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "armed": {
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "armed"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "armed": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "armed"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_rec_input`

**Profile:** `core` · **Hints:** idempotent

Set a track's record input index (I_RECINPUT; mono in n, stereo, MIDI, etc. — see the REAPER record-input encoding).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `input` | integer | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `input`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "input": {
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "input"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "input": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "input"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_rec_mode`

**Profile:** `core` · **Hints:** idempotent

Set a track's record mode (I_RECMODE: 0 input, 1 stereo out, 2 none/monitor-only, 3 stereo out w/latency comp, 4 midi output, 5 mono out, 6 mono out w/latency comp, 7 midi overdub, 8 midi replace).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `mode` | integer | yes | range [0, 8] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `mode`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "mode": {
        "maximum": 8,
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "mode"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "mode": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "mode"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_rec_mon`

**Profile:** `core` · **Hints:** idempotent

Set a track's record monitoring mode (I_RECMON: 0 off, 1 on, 2 auto).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `mode` | integer | yes | range [0, 2] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `mode`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "mode": {
        "maximum": 2,
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "mode"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "mode": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "mode"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_solo`

**Profile:** `core` · **Hints:** idempotent

Solo or unsolo a track (I_SOLO; solo = solo-in-place).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `soloed` | boolean | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `soloed`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "soloed": {
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "soloed"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "soloed": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "soloed"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_state_chunk`

**Profile:** `core` · **Hints:** **destructive**

Replace a track's entire state from a raw REAPER <TRACK ...> chunk (FX, routing, envelopes, items — the write complement of reaper://track/{index}/chunk). DESTRUCTIVE wholesale replace: an elicitation-capable client confirms via an MCP elicitation round-trip; others pass confirm:true. dryRun (preview) returns a unified diff of current-vs-proposed and changes nothing. The chunk is structurally validated first; an unchanged proposal is a no-op.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `chunk` | string | yes | — |
| `confirm` | boolean | no | default `false` |
| `context` | integer | no | default `3`; min 0 |
| `dryRun` | boolean | no | default `false` |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `addedLines`, `applied`, `changed`, `detail`, `diff`, `dryRun`, `error`, `note`, `ok`, `remediation`, `removedLines`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "chunk": {
        "type": "string"
      },
      "confirm": {
        "default": false,
        "type": "boolean"
      },
      "context": {
        "default": 3,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "chunk"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "addedLines": {
        "type": "integer"
      },
      "applied": {
        "type": "boolean"
      },
      "changed": {
        "type": "boolean"
      },
      "detail": {
        "type": "string"
      },
      "diff": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "note": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "remediation": {
        "type": "string"
      },
      "removedLines": {
        "type": "integer"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "track"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_width`

**Profile:** `core` · **Hints:** idempotent

Set a track's stereo/pan width (D_WIDTH, -1..1; 1 = full width).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |
| `width` | number | yes | range [-1, 1] |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `width`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "width": {
        "maximum": 1,
        "minimum": -1,
        "type": "number"
      }
    },
    "required": [
      "track",
      "width"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "width": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "width"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.unfreeze`

**Profile:** `core` · **Hints:** idempotent

Unfreeze a track (restore the items and FX saved by track.freeze). No-op if the track is not frozen.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxCount`, `itemCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxCount": {
        "type": "integer"
      },
      "itemCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `transport.get_state`

**Profile:** `core` · **Hints:** read-only, idempotent

Get transport state: play/pause/record, edit cursor, play position, tempo, loop.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `cursorSec`, `loop`, `playPositionSec`, `playState`, `tempo`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "cursorSec": {
        "type": "number"
      },
      "loop": {
        "type": "boolean"
      },
      "playPositionSec": {
        "type": "number"
      },
      "playState": {
        "enum": [
          "stopped",
          "playing",
          "paused",
          "recording"
        ],
        "type": "string"
      },
      "tempo": {
        "type": "number"
      }
    },
    "required": [
      "playState",
      "cursorSec",
      "tempo",
      "loop"
    ],
    "type": "object"
  }
}
```
</details>

#### `transport.set`

**Profile:** `core` · **Hints:** mutating

Control transport: play \| stop \| pause \| record \| goto(sec).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `action` | enum | yes | one of: `play`, `stop`, `pause`, `record`, `goto` |
| `sec` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `action`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "action": {
        "enum": [
          "play",
          "stop",
          "pause",
          "record",
          "goto"
        ],
        "type": "string"
      },
      "sec": {
        "type": "number"
      }
    },
    "required": [
      "action"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "action": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `transport.set_metronome`

**Profile:** `core` · **Hints:** idempotent

Turn the metronome (click) on or off. Wraps REAPER action 40364 and reads its toggle state back so it lands on the requested state regardless of the starting state.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `enabled` | boolean | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `enabled`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "enabled": {
        "type": "boolean"
      }
    },
    "required": [
      "enabled"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "enabled": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "enabled"
    ],
    "type": "object"
  }
}
```
</details>

#### `transport.set_playrate`

**Profile:** `core` · **Hints:** idempotent

Set the master play rate (0.25..4.0; 1.0 = normal speed).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `rate` | number | yes | range [0.25, 4] |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `rate`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "rate": {
        "maximum": 4,
        "minimum": 0.25,
        "type": "number"
      }
    },
    "required": [
      "rate"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "rate": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "rate"
    ],
    "type": "object"
  }
}
```
</details>

#### `transport.set_repeat`

**Profile:** `core` · **Hints:** idempotent

Turn the transport repeat/loop on or off.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `enabled` | boolean | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `loop`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "enabled": {
        "type": "boolean"
      }
    },
    "required": [
      "enabled"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "loop": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "loop"
    ],
    "type": "object"
  }
}
```
</details>

### Spatial / immersive

_Immersive/spatial audio: channel beds, ambisonic encode/decode, surround panners, scene rotation, binaural monitoring, and live head-tracking._

#### `spatial.add_binaural_monitor`

**Profile:** `spatial` · **Hints:** mutating

Build a non-destructive 2-channel binaural monitor bus so an agent can audition an immersive mix on headphones. mode 'ambisonic' (default): a labeled monitor bus is fed by a multichannel send off the given HOA scene track, and an ambisonic->binaural decoder (IEM BinauralDecoder by default; SPARTA/ATK fallback) is inserted with its HOA input pins identity-wired and its binaural output on channels 1/2. mode 'object': each objectTracks entry is sent to its own channel and an object binauraliser (SPARTA Binauraliser) is inserted. The source track is left untouched (solo the monitor, or mute the scene's master send, to audition in isolation). When headTracking is true a scene rotator (ambisonic) — or the binauraliser's own yaw/pitch/roll (object) — is driven live by an extension-hosted OSC/UDP listener: point a head-tracker (phone/IMU/Supperware/nvsonic/Unity) at the reported oscPort. Supported OSC: /yaw //pitch //roll floats, a 3-float /ypr (or /orientation//euler), or a 4-float /quaternion (w,x,y,z). The rotator's yaw/pitch/roll param indices are also returned so you can automate them with an envelope, and the OSC addresses are reported so you can instead target the plug-in's own OSC port. Call spatial.stop_head_tracking to release the port.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `createSend` | boolean | no | default `true` |
| `decoderPlugin` | string | no | — |
| `headTracking` | boolean | no | default `false` |
| `index` | integer | no | min 0 |
| `invertPitch` | boolean | no | default `false` |
| `invertRoll` | boolean | no | default `false` |
| `invertYaw` | boolean | no | default `false` |
| `mode` | enum | no | one of: `ambisonic`, `object`; default `"ambisonic"` |
| `monitorName` | string | no | — |
| `objectTracks` | array&lt;integer&gt; | no | — |
| `order` | integer | no | range [1, 7] |
| `oscBind` | string | no | default `"127.0.0.1"` |
| `oscPort` | integer | no | range [0, 65535] |
| `rotatorPlugin` | string | no | — |
| `suite` | enum | no | one of: `auto`, `IEM`, `SPARTA`, `ambiX`, `ATK`; default `"auto"` |
| `track` | integer | yes | min 0 |
| `wirePins` | boolean | no | default `true` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `automationParams`, `busChannels`, `decoder`, `decoderFx`, `headTracking`, `hoaChannels`, `inputPins`, `mode`, `monitorName`, `monitorTrack`, `ok`, `order`, `rotator`, `rotatorFx`, `sends`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "createSend": {
        "default": true,
        "type": "boolean"
      },
      "decoderPlugin": {
        "type": "string"
      },
      "headTracking": {
        "default": false,
        "type": "boolean"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "invertPitch": {
        "default": false,
        "type": "boolean"
      },
      "invertRoll": {
        "default": false,
        "type": "boolean"
      },
      "invertYaw": {
        "default": false,
        "type": "boolean"
      },
      "mode": {
        "default": "ambisonic",
        "enum": [
          "ambisonic",
          "object"
        ],
        "type": "string"
      },
      "monitorName": {
        "type": "string"
      },
      "objectTracks": {
        "items": {
          "minimum": 0,
          "type": "integer"
        },
        "type": "array"
      },
      "order": {
        "maximum": 7,
        "minimum": 1,
        "type": "integer"
      },
      "oscBind": {
        "default": "127.0.0.1",
        "type": "string"
      },
      "oscPort": {
        "maximum": 65535,
        "minimum": 0,
        "type": "integer"
      },
      "rotatorPlugin": {
        "type": "string"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "IEM",
          "SPARTA",
          "ambiX",
          "ATK"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "wirePins": {
        "default": true,
        "type": "boolean"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "automationParams": {
        "type": "object"
      },
      "busChannels": {
        "type": "integer"
      },
      "decoder": {
        "type": "string"
      },
      "decoderFx": {
        "type": "integer"
      },
      "headTracking": {
        "type": "object"
      },
      "hoaChannels": {
        "type": [
          "integer",
          "null"
        ]
      },
      "inputPins": {
        "type": "integer"
      },
      "mode": {
        "type": "string"
      },
      "monitorName": {
        "type": "string"
      },
      "monitorTrack": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "order": {
        "type": [
          "integer",
          "null"
        ]
      },
      "rotator": {
        "type": [
          "string",
          "null"
        ]
      },
      "rotatorFx": {
        "type": "integer"
      },
      "sends": {
        "type": "array"
      },
      "warnings": {
        "items": {
          "type": "string"
        },
        "type": "array"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.add_surround_panner`

**Profile:** `spatial` · **Hints:** mutating

Add REAPER's native surround panner (ReaSurroundPan) to a track and optionally set its NUMCHANNELS (sources it pans) and NUMSPEAKERS (speaker array). Returns the FX index plus the discovered parameter names so a caller can drive per-source positions.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `channels` | integer | no | range [1, 128] |
| `plugin` | string | no | default `"ReaSurroundPan"` |
| `setTrackChannels` | integer | no | range [2, 128] |
| `speakers` | integer | no | range [1, 128] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxIndex`, `name`, `numChannels`, `numSpeakers`, `paramCount`, `params`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "channels": {
        "maximum": 128,
        "minimum": 1,
        "type": "integer"
      },
      "plugin": {
        "default": "ReaSurroundPan",
        "type": "string"
      },
      "setTrackChannels": {
        "maximum": 128,
        "minimum": 2,
        "type": "integer"
      },
      "speakers": {
        "maximum": 128,
        "minimum": 1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxIndex": {
        "type": "integer"
      },
      "name": {
        "type": "string"
      },
      "numChannels": {
        "type": "string"
      },
      "numSpeakers": {
        "type": "string"
      },
      "paramCount": {
        "type": "integer"
      },
      "params": {
        "items": {
          "properties": {
            "index": {
              "type": "integer"
            },
            "name": {
              "type": "string"
            }
          },
          "type": "object"
        },
        "type": "array"
      }
    },
    "required": [
      "fxIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.ambisonic_decode`

**Profile:** `spatial` · **Hints:** mutating

Insert (or drive an existing) ambisonic decoder (IEM SimpleDecoder/AllRADecoder by default; SPARTA AmbiDEC on request) on a HOA track to produce loudspeaker feeds, and identity-wire the track's HOA channels 0..N-1 onto the decoder's input pins. Optionally set the output channel count. Order/Normalization are reported, not blind-set. NOTE: the loudspeaker layout for config-file decoders is chosen in the plug-in UI / a preset — this wires I/O and reports the params; it does not synthesize a decoder matrix.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | no | min 0 |
| `normalization` | enum | no | one of: `SN3D`, `N3D`, `FuMa`; default `"SN3D"` |
| `order` | integer | no | range [1, 7] |
| `outputChannels` | integer | no | range [2, 128] |
| `plugin` | string | no | — |
| `suite` | enum | no | one of: `auto`, `IEM`, `SPARTA`, `ambiX`, `ATK`; default `"auto"` |
| `track` | integer | yes | min 0 |
| `wirePins` | boolean | no | default `true` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `decoder`, `fxIndex`, `hoaChannels`, `inputPins`, `normalizationParam`, `note`, `ok`, `orderParam`, `outputPins`, `pinMap`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "normalization": {
        "default": "SN3D",
        "enum": [
          "SN3D",
          "N3D",
          "FuMa"
        ],
        "type": "string"
      },
      "order": {
        "maximum": 7,
        "minimum": 1,
        "type": "integer"
      },
      "outputChannels": {
        "maximum": 128,
        "minimum": 2,
        "type": "integer"
      },
      "plugin": {
        "type": "string"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "IEM",
          "SPARTA",
          "ambiX",
          "ATK"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "wirePins": {
        "default": true,
        "type": "boolean"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "decoder": {
        "type": "string"
      },
      "fxIndex": {
        "type": "integer"
      },
      "hoaChannels": {
        "type": [
          "integer",
          "null"
        ]
      },
      "inputPins": {
        "type": "integer"
      },
      "normalizationParam": {
        "type": [
          "object",
          "null"
        ]
      },
      "note": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "orderParam": {
        "type": [
          "object",
          "null"
        ]
      },
      "outputPins": {
        "type": "integer"
      },
      "pinMap": {
        "type": "array"
      },
      "warnings": {
        "type": "array"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.ambisonic_encode`

**Profile:** `spatial` · **Hints:** mutating

Encode a track to ambisonics: set the track channel count for the order ((order+1)^2, even-padded), insert an installed encoder (IEM StereoEncoder by default; SPARTA/ATK/ambiX on request), point the source at (azimuthDeg, elevationDeg), and pin-wire the encoder's ACN outputs onto channels 0..N-1. Order Setting & Normalization (SN3D default; N3D/FuMa) are REPORTED for exact control rather than blind-set (their enum indices vary per plug-in). Pass source (1-based) to target one input of a multi-source encoder (e.g. MultiEncoder Azimuth 3).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `azimuthDeg` | number | no | — |
| `elevationDeg` | number | no | — |
| `fx` | integer | no | min 0 |
| `normalization` | enum | no | one of: `SN3D`, `N3D`, `FuMa`; default `"SN3D"` |
| `order` | integer | yes | range [1, 7] |
| `plugin` | string | no | — |
| `rollDeg` | number | no | — |
| `setChannels` | boolean | no | default `true` |
| `source` | integer | no | min 1 |
| `suite` | enum | no | one of: `auto`, `IEM`, `SPARTA`, `ATK`, `ambiX`; default `"auto"` |
| `track` | integer | yes | min 0 |
| `widthDeg` | number | no | — |
| `wirePins` | boolean | no | default `true` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `encoder`, `fxIndex`, `hoaChannels`, `normalizationParam`, `ok`, `order`, `orderParam`, `outputPins`, `padded`, `pinMap`, `positions`, `requestedNormalization`, `trackChannels`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "azimuthDeg": {
        "type": "number"
      },
      "elevationDeg": {
        "type": "number"
      },
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "normalization": {
        "default": "SN3D",
        "enum": [
          "SN3D",
          "N3D",
          "FuMa"
        ],
        "type": "string"
      },
      "order": {
        "maximum": 7,
        "minimum": 1,
        "type": "integer"
      },
      "plugin": {
        "type": "string"
      },
      "rollDeg": {
        "type": "number"
      },
      "setChannels": {
        "default": true,
        "type": "boolean"
      },
      "source": {
        "minimum": 1,
        "type": "integer"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "IEM",
          "SPARTA",
          "ATK",
          "ambiX"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "widthDeg": {
        "type": "number"
      },
      "wirePins": {
        "default": true,
        "type": "boolean"
      }
    },
    "required": [
      "track",
      "order"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "encoder": {
        "type": "string"
      },
      "fxIndex": {
        "type": "integer"
      },
      "hoaChannels": {
        "type": "integer"
      },
      "normalizationParam": {
        "type": [
          "object",
          "null"
        ]
      },
      "ok": {
        "type": "boolean"
      },
      "order": {
        "type": "integer"
      },
      "orderParam": {
        "type": [
          "object",
          "null"
        ]
      },
      "outputPins": {
        "type": "integer"
      },
      "padded": {
        "type": "boolean"
      },
      "pinMap": {
        "type": "array"
      },
      "positions": {
        "type": "array"
      },
      "requestedNormalization": {
        "type": "string"
      },
      "trackChannels": {
        "type": "integer"
      },
      "warnings": {
        "type": "array"
      }
    },
    "required": [
      "ok",
      "order",
      "hoaChannels",
      "trackChannels"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.assign_to_bed`

**Profile:** `spatial` · **Hints:** mutating

Route a source track into a bed bus with a multichannel send landing at a destination channel offset. channels=1 mono, 2 stereo, 4/6/8/... wider. Use destChannel to place a stem at a specific speaker (0-based); monoToChannel sums a stereo source into one channel.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bed` | integer | yes | min 0 |
| `channels` | integer | no | default `2`; range [1, 64] |
| `db` | number | no | — |
| `destChannel` | integer | no | default `0`; min 0 |
| `monoToChannel` | boolean | no | default `false` |
| `srcChannel` | integer | no | default `0`; min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `bed`, `channels`, `destChannel`, `dstChanEnc`, `sendIndex`, `srcChanEnc`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bed": {
        "minimum": 0,
        "type": "integer"
      },
      "channels": {
        "default": 2,
        "maximum": 64,
        "minimum": 1,
        "type": "integer"
      },
      "db": {
        "type": "number"
      },
      "destChannel": {
        "default": 0,
        "minimum": 0,
        "type": "integer"
      },
      "monoToChannel": {
        "default": false,
        "type": "boolean"
      },
      "srcChannel": {
        "default": 0,
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "bed"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "bed": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "destChannel": {
        "type": "integer"
      },
      "dstChanEnc": {
        "type": "integer"
      },
      "sendIndex": {
        "type": "integer"
      },
      "srcChanEnc": {
        "type": "integer"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "sendIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.author_object_bed`

**Profile:** `spatial` · **Hints:** idempotent

Validate (and optionally establish) the bed+object topology an ADM object-audio deliverable needs, then emit the object map spatial.export_adm consumes — the idempotent 'manage my Atmos session for ADM export' layer above the one-shot spatial.setup_immersive_session. Give a bedTrack (a DirectSpeakers bus) + a bedLayout (5.1 \| 7.1 \| 7.1.2 \| 7.1.4 \| 9.1.6 \| 22.2; 7.1.2 is the Atmos-canonical bed) + objectTracks (mono object tracks, each spatialized by a panner whose azimuth/elevation is the object's position metadata). Reports, per object, a stable 1-based objectNumber, its channel count, whether a positional panner was found, and its current position; reports the bed's layout/width/labels; and a 'ready' flag = the session is export_adm-ready. dryRun (the DEFAULT) only inspects + reports what apply would change. Apply (dryRun:false) performs the minimal safe wiring in ONE undo block: build the bed if createBed and no bedTrack, widen the bed track to the layout width, and (renameObjects) rename object tracks 'Object N'. It never touches object audio or panner positions.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bedLayout` | enum | no | one of: `5.1`, `7.1`, `7.1.2`, `7.1.4`, `9.1.6`, `22.2`; default `"7.1.2"` |
| `bedTrack` | integer | no | min 0 |
| `createBed` | boolean | no | default `false` |
| `dryRun` | boolean | no | default `true` |
| `objectTracks` | array&lt;integer&gt; | no | — |
| `renameObjects` | boolean | no | default `false` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `actions`, `bed`, `detail`, `dryRun`, `error`, `objects`, `ok`, `ready`, `remediation`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bedLayout": {
        "default": "7.1.2",
        "enum": [
          "5.1",
          "7.1",
          "7.1.2",
          "7.1.4",
          "9.1.6",
          "22.2"
        ],
        "type": "string"
      },
      "bedTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "createBed": {
        "default": false,
        "type": "boolean"
      },
      "dryRun": {
        "default": true,
        "type": "boolean"
      },
      "objectTracks": {
        "items": {
          "minimum": 0,
          "type": "integer"
        },
        "type": "array"
      },
      "renameObjects": {
        "default": false,
        "type": "boolean"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "actions": {
        "type": "array"
      },
      "bed": {
        "type": "object"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "objects": {
        "type": "array"
      },
      "ok": {
        "type": "boolean"
      },
      "ready": {
        "type": "boolean"
      },
      "remediation": {
        "type": "string"
      },
      "warnings": {
        "items": {
          "type": "string"
        },
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `spatial.beamform`

**Profile:** `spatial` · **Hints:** mutating

Extract a steerable directional signal (virtual microphone) from a HOA scene by inserting (or driving) a beamformer — SPARTA Beamformer by default — pointing at (azimuthDeg, elevationDeg) and identity-wiring the track's HOA channels onto its inputs. The beam order / pattern is a choice / continuous param whose enum differs per build, so it is reported for exact control; azimuth / elevation are written (degrees, clamped to each param's range). Fails closed with an install hint if no beamformer is present.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `azimuthDeg` | number | no | — |
| `beam` | number | no | — |
| `elevationDeg` | number | no | — |
| `fx` | integer | no | min 0 |
| `plugin` | string | no | — |
| `suite` | enum | no | one of: `auto`, `SPARTA`; default `"auto"` |
| `track` | integer | yes | min 0 |
| `wirePins` | boolean | no | default `true` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `automationParams`, `beamformer`, `fxIndex`, `inputPins`, `ok`, `pinMap`, `set`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "azimuthDeg": {
        "type": "number"
      },
      "beam": {
        "type": "number"
      },
      "elevationDeg": {
        "type": "number"
      },
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "plugin": {
        "type": "string"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "SPARTA"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "wirePins": {
        "default": true,
        "type": "boolean"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "automationParams": {
        "type": "object"
      },
      "beamformer": {
        "type": "string"
      },
      "fxIndex": {
        "type": "integer"
      },
      "inputPins": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "pinMap": {
        "type": "array"
      },
      "set": {
        "type": "array"
      },
      "warnings": {
        "type": "array"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.build_bed`

**Profile:** `spatial` · **Hints:** mutating

Create a channel-based bed bus (5.1 \| 7.1 \| 7.1.4 \| 9.1.6 \| 22.2): inserts a multichannel track with the right channel count, names it, and returns the speaker channel map (ready for stem assignment via spatial.assign_to_bed). Optionally make it a folder parent.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `busName` | string | no | — |
| `folder` | boolean | no | default `false` |
| `index` | integer | no | min 0 |
| `layout` | enum | yes | one of: `5.1`, `7.1`, `7.1.4`, `9.1.6`, `22.2` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `busTrack`, `channelMap`, `channels`, `layout`, `name`, `note`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "busName": {
        "type": "string"
      },
      "folder": {
        "default": false,
        "type": "boolean"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "layout": {
        "enum": [
          "5.1",
          "7.1",
          "7.1.4",
          "9.1.6",
          "22.2"
        ],
        "type": "string"
      }
    },
    "required": [
      "layout"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "busTrack": {
        "type": "integer"
      },
      "channelMap": {
        "items": {
          "properties": {
            "channel": {
              "type": "integer"
            },
            "label": {
              "type": "string"
            },
            "lfe": {
              "type": "boolean"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "channels": {
        "type": "integer"
      },
      "layout": {
        "type": "string"
      },
      "name": {
        "type": "string"
      },
      "note": {
        "type": "string"
      }
    },
    "required": [
      "busTrack",
      "channels",
      "layout",
      "channelMap"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.convert_format`

**Profile:** `spatial` · **Hints:** mutating

Convert a HOA track between ambisonic normalizations / formats via the bundled zero-dependency JSFX 'MCP Ambisonic Format Converter'. SN3D<->N3D is an exact per-degree gain for ANY order; ambiX(ACN/SN3D)<->FuMa is first-order / B-format ONLY (reorder + the W 1/sqrt2 scaling) — higher-order FuMa has no single agreed convention and is refused (keep HOA in SN3D/N3D). ambiX is treated as ACN/SN3D. Same from/to is a no-op (no FX inserted). N3D<->FuMa: convert via SN3D.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `from` | enum | yes | one of: `SN3D`, `N3D`, `FuMa`, `ambiX` |
| `fx` | integer | no | min 0 |
| `to` | enum | yes | one of: `SN3D`, `N3D`, `FuMa`, `ambiX` |
| `track` | integer | yes | min 0 |
| `wirePins` | boolean | no | default `true` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `converter`, `from`, `fxIndex`, `mode`, `modeParam`, `noop`, `note`, `ok`, `pinMap`, `to`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "from": {
        "enum": [
          "SN3D",
          "N3D",
          "FuMa",
          "ambiX"
        ],
        "type": "string"
      },
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "to": {
        "enum": [
          "SN3D",
          "N3D",
          "FuMa",
          "ambiX"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "wirePins": {
        "default": true,
        "type": "boolean"
      }
    },
    "required": [
      "track",
      "from",
      "to"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "converter": {
        "type": "string"
      },
      "from": {
        "type": "string"
      },
      "fxIndex": {
        "type": "integer"
      },
      "mode": {
        "type": "integer"
      },
      "modeParam": {
        "type": [
          "integer",
          "null"
        ]
      },
      "noop": {
        "type": "boolean"
      },
      "note": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "pinMap": {
        "type": "array"
      },
      "to": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      }
    },
    "required": [
      "ok",
      "from",
      "to"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.convert_order`

**Profile:** `spatial` · **Hints:** mutating

Change a HOA track's ambisonic order by resizing its ACN channel bus. DOWN-order truncates the higher-degree ACN channels — an exact operation: the retained lower orders are a valid lower-order scene. UP-order zero-pads with silent higher-degree channels — no spatial detail is invented (honest zero-pad; re-encode sources for real HOA gain). Pure channel-count + identity routing, no FX, no matrix. fromOrder is inferred from the current channel count when omitted.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fromOrder` | integer | no | range [1, 7] |
| `normalization` | enum | no | one of: `SN3D`, `N3D`, `FuMa`; default `"SN3D"` |
| `toOrder` | integer | yes | range [1, 7] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `channels`, `direction`, `fromOrder`, `hoaChannels`, `ok`, `padded`, `previousChannels`, `toOrder`, `track`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fromOrder": {
        "maximum": 7,
        "minimum": 1,
        "type": "integer"
      },
      "normalization": {
        "default": "SN3D",
        "enum": [
          "SN3D",
          "N3D",
          "FuMa"
        ],
        "type": "string"
      },
      "toOrder": {
        "maximum": 7,
        "minimum": 1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "toOrder"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channels": {
        "type": "integer"
      },
      "direction": {
        "type": "string"
      },
      "fromOrder": {
        "type": "integer"
      },
      "hoaChannels": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "padded": {
        "type": "boolean"
      },
      "previousChannels": {
        "type": "integer"
      },
      "toOrder": {
        "type": "integer"
      },
      "track": {
        "type": "integer"
      },
      "warnings": {
        "type": "array"
      }
    },
    "required": [
      "ok",
      "toOrder",
      "hoaChannels",
      "channels"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.detect_spatial_suites`

**Profile:** `spatial` · **Hints:** read-only, idempotent

Enumerate the ambisonic/spatial FX actually installed on this machine (IEM, SPARTA, ATK, ambiX, Blue Ripple) and classify each into a role (encoder/decoder/rotator/binaural). Returns the preferred encoder/decoder/rotator/binaural per the IEM>SPARTA>ATK priority so the other spatial tools know what they can drive. Instantiates nothing.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `preferred`, `suites`, `totalInstalledFx`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "preferred": {
        "type": "object"
      },
      "suites": {
        "items": {
          "properties": {
            "installed": {
              "type": "boolean"
            },
            "plugins": {
              "items": {
                "properties": {
                  "ident": {
                    "type": "string"
                  },
                  "name": {
                    "type": "string"
                  },
                  "role": {
                    "type": "string"
                  }
                },
                "type": "object"
              },
              "type": "array"
            },
            "suite": {
              "type": "string"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "totalInstalledFx": {
        "type": "integer"
      }
    },
    "required": [
      "suites"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.get_scene_info`

**Profile:** `spatial` · **Hints:** read-only, idempotent

Read-only: describe a track as an immersive / ambisonic bus. Infers the ambisonic order from the channel count ((order+1)^2, ACN), reports the real ACN channel count vs the even-padded track width, and lists any spatial-suite FX (encoders / decoders / rotators / binaural) on the track with their detected suite + role. Normalization is NOT signal-detectable, so it is reported as the project convention (ACN / SN3D), not measured.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `channels`, `convention`, `hoaChannels`, `inferredOrder`, `interpretation`, `ok`, `padded`, `spatialFx`, `surplusChannels`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channels": {
        "type": "integer"
      },
      "convention": {
        "type": "string"
      },
      "hoaChannels": {
        "type": [
          "integer",
          "null"
        ]
      },
      "inferredOrder": {
        "type": "integer"
      },
      "interpretation": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "padded": {
        "type": "boolean"
      },
      "spatialFx": {
        "type": "array"
      },
      "surplusChannels": {
        "type": "integer"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "channels",
      "inferredOrder"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.get_surround_state`

**Profile:** `spatial` · **Hints:** read-only, idempotent

Read a ReaSurroundPan instance's NUMCHANNELS/NUMSPEAKERS and every parameter (index, name, value, normalized, min, max). This is the source of truth for the panner's parameter layout — use it to discover which params control each source's position.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | no | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxIndex`, `name`, `numChannels`, `numSpeakers`, `paramCount`, `params`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxIndex": {
        "type": "integer"
      },
      "name": {
        "type": "string"
      },
      "numChannels": {
        "type": "string"
      },
      "numSpeakers": {
        "type": "string"
      },
      "paramCount": {
        "type": "integer"
      },
      "params": {
        "items": {
          "properties": {
            "index": {
              "type": "integer"
            },
            "max": {
              "type": "number"
            },
            "min": {
              "type": "number"
            },
            "name": {
              "type": "string"
            },
            "normalized": {
              "type": "number"
            },
            "value": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      }
    },
    "required": [
      "fxIndex",
      "paramCount",
      "params"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.mirror_scene`

**Profile:** `spatial` · **Hints:** mutating

Reflect a HOA scene across a principal plane via the bundled zero-dependency JSFX 'MCP Ambisonic Scene Mirror', which applies the exact per-ACN-channel sign pattern of that reflection (order-independent, any order up to 7th). plane: 'left-right' swaps L<->R, 'front-back' swaps front<->back, 'up-down' swaps up<->down. Complements spatial.rotate_scene — a rotation is not a reflection; mirroring flips the scene's handedness.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | no | min 0 |
| `plane` | enum | no | one of: `left-right`, `front-back`, `up-down`; default `"left-right"` |
| `track` | integer | yes | min 0 |
| `wirePins` | boolean | no | default `true` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxIndex`, `mirror`, `ok`, `pinMap`, `plane`, `planeIndex`, `planeParam`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "plane": {
        "default": "left-right",
        "enum": [
          "left-right",
          "front-back",
          "up-down"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "wirePins": {
        "default": true,
        "type": "boolean"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxIndex": {
        "type": "integer"
      },
      "mirror": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "pinMap": {
        "type": "array"
      },
      "plane": {
        "type": "string"
      },
      "planeIndex": {
        "type": "integer"
      },
      "planeParam": {
        "type": [
          "integer",
          "null"
        ]
      },
      "warnings": {
        "type": "array"
      }
    },
    "required": [
      "ok",
      "plane"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.orchestrate_sends`

**Profile:** `spatial` · **Hints:** idempotent

Auto-orchestrate the object + bed send layout into an external Dolby Atmos Renderer / DAPS input bus on the canonical 1-based input roster: the bed lands on renderer channels 1..bedW, then each mono object takes one subsequent channel (bed-first, matching the ADM/DAMF roster). objectTracks is an explicit ordered list; omit it to auto-discover mono 'Object N' tracks (the convention setup_immersive_session / author_object_bed create) in N order. IDEMPOTENT: an existing send that already lands at the right channel is REUSED, a send from the right source at the wrong channel is FIXED in place, and only missing sends are added — re-running never stacks duplicates. Stray sends into the bus (no roster slot) are reported; prune:true removes them. FAILS CLOSED on the Dolby master limits (<=128 renderer channels, <=118 objects). dryRun (the default) returns the plan without touching routing.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bedLayout` | enum | no | one of: `5.1`, `7.1`, `7.1.2`, `7.1.4`, `9.1.6`, `22.2`; default `"7.1.2"` |
| `bedTrack` | integer | no | min 0 |
| `db` | number | no | — |
| `dryRun` | boolean | no | default `true` |
| `objectTracks` | array&lt;integer&gt; | no | — |
| `prune` | boolean | no | default `false` |
| `rendererTrack` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `actions`, `bedChannels`, `bedLayout`, `bedTrack`, `constraints`, `counts`, `detail`, `discovered`, `dryRun`, `error`, `inputs`, `objectCount`, `ok`, `remediation`, `rendererTrack`, `strays`, `totalChannels`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bedLayout": {
        "default": "7.1.2",
        "enum": [
          "5.1",
          "7.1",
          "7.1.2",
          "7.1.4",
          "9.1.6",
          "22.2"
        ],
        "type": "string"
      },
      "bedTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "db": {
        "type": "number"
      },
      "dryRun": {
        "default": true,
        "type": "boolean"
      },
      "objectTracks": {
        "items": {
          "minimum": 0,
          "type": "integer"
        },
        "type": "array"
      },
      "prune": {
        "default": false,
        "type": "boolean"
      },
      "rendererTrack": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "rendererTrack"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "actions": {
        "type": "array"
      },
      "bedChannels": {
        "type": "integer"
      },
      "bedLayout": {
        "type": "string"
      },
      "bedTrack": {
        "type": "integer"
      },
      "constraints": {
        "type": "array"
      },
      "counts": {
        "type": "object"
      },
      "detail": {
        "type": "string"
      },
      "discovered": {
        "type": "boolean"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "inputs": {
        "type": "array"
      },
      "objectCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "remediation": {
        "type": "string"
      },
      "rendererTrack": {
        "type": "integer"
      },
      "strays": {
        "type": "array"
      },
      "totalChannels": {
        "type": "integer"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `spatial.rotate_scene`

**Profile:** `spatial` · **Hints:** mutating

Insert (or drive an existing) ambisonic scene rotator (IEM SceneRotator by default) on a HOA track and set yaw/pitch/roll in degrees. Identity-wires the rotator's HOA in/out pins onto the track channels. Returns the yaw/pitch/roll param indices so an agent can automate them (head-tracking / picture-locked yaw) with an envelope.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | no | min 0 |
| `pitchDeg` | number | no | — |
| `plugin` | string | no | — |
| `rollDeg` | number | no | — |
| `suite` | enum | no | one of: `auto`, `IEM`, `SPARTA`, `ambiX`, `ATK`; default `"auto"` |
| `track` | integer | yes | min 0 |
| `wirePins` | boolean | no | default `true` |
| `yawDeg` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `automationParams`, `fxIndex`, `ok`, `pinMap`, `rotator`, `set`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "pitchDeg": {
        "type": "number"
      },
      "plugin": {
        "type": "string"
      },
      "rollDeg": {
        "type": "number"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "IEM",
          "SPARTA",
          "ambiX",
          "ATK"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "wirePins": {
        "default": true,
        "type": "boolean"
      },
      "yawDeg": {
        "type": "number"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "automationParams": {
        "type": "object"
      },
      "fxIndex": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "pinMap": {
        "type": "array"
      },
      "rotator": {
        "type": "string"
      },
      "set": {
        "type": "array"
      },
      "warnings": {
        "type": "array"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.set_distance`

**Profile:** `spatial` · **Hints:** mutating

Add source distance, shoebox-room size, and early-reflection depth to a HOA scene by inserting (or driving) IEM RoomEncoder. distanceM places the source that many metres from the listener along (azimuthDeg, elevationDeg) — the source X/Y/Z params use RoomEncoder's own room frame (+X=front, +Y=left, +Z=up, +az=left) with the listener pinned to the room centre, each mapped onto the plug-in's own metric range; roomSizeM sets the room dimensions; reflectionOrder sets the reflection depth. Params are discovered by name and their automation indices reported; unmatched params are warned, not guessed. Fails closed if RoomEncoder is not installed.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `azimuthDeg` | number | no | — |
| `distanceM` | number | no | min 0 |
| `elevationDeg` | number | no | — |
| `fx` | integer | no | min 0 |
| `plugin` | string | no | — |
| `reflectionOrder` | integer | no | min 0 |
| `roomSizeM` | number | no | min 0 |
| `suite` | enum | no | one of: `auto`, `IEM`; default `"auto"` |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `automationParams`, `fxIndex`, `ok`, `roomEncoder`, `set`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "azimuthDeg": {
        "type": "number"
      },
      "distanceM": {
        "minimum": 0,
        "type": "number"
      },
      "elevationDeg": {
        "type": "number"
      },
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "plugin": {
        "type": "string"
      },
      "reflectionOrder": {
        "minimum": 0,
        "type": "integer"
      },
      "roomSizeM": {
        "minimum": 0,
        "type": "number"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "IEM"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "automationParams": {
        "type": "object"
      },
      "fxIndex": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "roomEncoder": {
        "type": "string"
      },
      "set": {
        "type": "array"
      },
      "warnings": {
        "type": "array"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.set_source_position`

**Profile:** `spatial` · **Hints:** idempotent

Position a source in the surround panner. Give x/y in [-1,1] (or azimuthDeg[/elevationDeg]) and optional z height; values are written normalized ([0,1], centre 0.5) so any param range is safe. Param indices are discovered by name; pass paramX/paramY/paramZ (from spatial.get_surround_state) to drive them deterministically.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `azimuthDeg` | number | no | — |
| `elevationDeg` | number | no | — |
| `fx` | integer | no | min 0 |
| `normalizedInput` | boolean | no | default `false` |
| `paramX` | integer | no | min 0 |
| `paramY` | integer | no | min 0 |
| `paramZ` | integer | no | min 0 |
| `source` | integer | no | default `0`; min 0 |
| `track` | integer | yes | min 0 |
| `x` | number | no | — |
| `y` | number | no | — |
| `z` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxIndex`, `message`, `ok`, `set`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "azimuthDeg": {
        "type": "number"
      },
      "elevationDeg": {
        "type": "number"
      },
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "normalizedInput": {
        "default": false,
        "type": "boolean"
      },
      "paramX": {
        "minimum": 0,
        "type": "integer"
      },
      "paramY": {
        "minimum": 0,
        "type": "integer"
      },
      "paramZ": {
        "minimum": 0,
        "type": "integer"
      },
      "source": {
        "default": 0,
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "x": {
        "type": "number"
      },
      "y": {
        "type": "number"
      },
      "z": {
        "type": "number"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxIndex": {
        "type": "integer"
      },
      "message": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "set": {
        "items": {
          "properties": {
            "axis": {
              "type": "string"
            },
            "normalized": {
              "type": "number"
            },
            "param": {
              "type": "integer"
            }
          },
          "type": "object"
        },
        "type": "array"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.set_track_channels`

**Profile:** `spatial` · **Hints:** idempotent

Set a track's channel count (2..128, even) and report what that count means in immersive terms (bed layout and/or ambisonic order). 16 = 9.1.6 or 3rd-order ambisonics, 12 = 7.1.4.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `channels` | integer | yes | range [2, 128] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ambisonicOrder`, `channels`, `interpretation`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "channels": {
        "maximum": 128,
        "minimum": 2,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "channels"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ambisonicOrder": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "interpretation": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "channels"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.setup_immersive_session`

**Profile:** `spatial` · **Hints:** **destructive**

One-call immersive session: build a bed (5.1\|7.1\|7.1.4\|9.1.6\|22.2), add N tagged mono object tracks, an optional binaural monitor, and the external Dolby Renderer / DAPS send layout (REAPER cannot author ADM BWF — this wires the topology a certified renderer consumes). DESTRUCTIVE on a non-empty project: an elicitation-capable client is asked to confirm via a real MCP elicitation round-trip; clients without elicitation pass confirm:true (fallback). dryRun returns the plan.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bed` | enum | no | one of: `5.1`, `7.1`, `7.1.4`, `9.1.6`, `22.2`; default `"7.1.4"` |
| `confirm` | boolean | no | default `false` |
| `dryRun` | boolean | no | default `false` |
| `monitor` | boolean | no | default `false` |
| `objectCount` | integer | no | default `0`; range [0, 128] |
| `rendererSends` | boolean | no | default `false` |
| `suite` | enum | no | one of: `auto`, `IEM`, `SPARTA`, `ATK`, `ambiX`; default `"auto"` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `bedBus`, `bedLayout`, `detail`, `diff`, `dryRun`, `error`, `monitor`, `objectTracks`, `ok`, `plan`, `remediation`, `rendererLayout`, `stepCount`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bed": {
        "default": "7.1.4",
        "enum": [
          "5.1",
          "7.1",
          "7.1.4",
          "9.1.6",
          "22.2"
        ],
        "type": "string"
      },
      "confirm": {
        "default": false,
        "type": "boolean"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "monitor": {
        "default": false,
        "type": "boolean"
      },
      "objectCount": {
        "default": 0,
        "maximum": 128,
        "minimum": 0,
        "type": "integer"
      },
      "rendererSends": {
        "default": false,
        "type": "boolean"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "IEM",
          "SPARTA",
          "ATK",
          "ambiX"
        ],
        "type": "string"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "bedBus": {
        "type": "integer"
      },
      "bedLayout": {
        "type": "string"
      },
      "detail": {
        "type": "string"
      },
      "diff": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "monitor": {
        "type": [
          "object",
          "null"
        ]
      },
      "objectTracks": {
        "type": "array"
      },
      "ok": {
        "type": "boolean"
      },
      "plan": {
        "type": "array"
      },
      "remediation": {
        "type": "string"
      },
      "rendererLayout": {
        "type": [
          "object",
          "null"
        ]
      },
      "stepCount": {
        "type": "integer"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `spatial.spatialize_stems`

**Profile:** `spatial` · **Hints:** idempotent

Spatialize source stems into a channel bed (target.layout) or an ambisonic scene (target.ambisonicOrder), placing each by a semantic descriptor (front-center, wide, overhead, side, rear, lfe, ...) or explicit {az,el}. Composes detect -> build_bed/set_channels -> per-source encode or surround-pan -> sends, in one undo block. dryRun returns the plan+diff. Idempotent: an existing bus of the same name is reused, not duplicated.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `busName` | string | no | — |
| `dryRun` | boolean | no | default `false` |
| `monitor` | boolean | no | default `false` |
| `placements` | array&lt;['string', 'object']&gt; | no | — |
| `sources` | array&lt;['integer', 'string']&gt; | yes | — |
| `suite` | enum | no | one of: `auto`, `IEM`, `SPARTA`, `ATK`, `ambiX`; default `"auto"` |
| `target` | object | yes | object — see full schema below |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `bus`, `busName`, `detail`, `diff`, `dryRun`, `error`, `monitor`, `ok`, `placements`, `plan`, `remediation`, `reusedBus`, `stepCount`, `target`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "busName": {
        "type": "string"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "monitor": {
        "default": false,
        "type": "boolean"
      },
      "placements": {
        "items": {
          "type": [
            "string",
            "object"
          ]
        },
        "type": "array"
      },
      "sources": {
        "items": {
          "type": [
            "integer",
            "string"
          ]
        },
        "minItems": 1,
        "type": "array"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "IEM",
          "SPARTA",
          "ATK",
          "ambiX"
        ],
        "type": "string"
      },
      "target": {
        "type": "object"
      }
    },
    "required": [
      "sources",
      "target"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "bus": {
        "type": "integer"
      },
      "busName": {
        "type": "string"
      },
      "detail": {
        "type": "string"
      },
      "diff": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "monitor": {
        "type": [
          "object",
          "null"
        ]
      },
      "ok": {
        "type": "boolean"
      },
      "placements": {
        "type": "array"
      },
      "plan": {
        "type": "array"
      },
      "remediation": {
        "type": "string"
      },
      "reusedBus": {
        "type": "boolean"
      },
      "stepCount": {
        "type": "integer"
      },
      "target": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `spatial.stereo_to_ambisonic`

**Profile:** `spatial` · **Hints:** idempotent

Upmix a mono/stereo source to a first/higher-order ambisonic SKETCH: set (order+1)^2 channels, insert an encoder, splay L/R at +/-spreadDeg (default 30), and report order + normalization. Explicitly a sketch (a decorrelated upmix, not a true soundfield capture). dryRun returns the plan.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `dryRun` | boolean | no | default `false` |
| `monitor` | boolean | no | default `false` |
| `normalization` | enum | no | one of: `SN3D`, `N3D`, `FuMa`; default `"SN3D"` |
| `order` | integer | no | default `1`; range [1, 7] |
| `source` | integer \| string | yes | — |
| `spreadDeg` | number | no | default `30` |
| `suite` | enum | no | one of: `auto`, `IEM`, `SPARTA`, `ATK`, `ambiX`; default `"auto"` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `channels`, `detail`, `diff`, `dryRun`, `encoder`, `error`, `monitor`, `normalization`, `normalizationParam`, `ok`, `order`, `orderParam`, `plan`, `positions`, `remediation`, `sketch`, `track`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "monitor": {
        "default": false,
        "type": "boolean"
      },
      "normalization": {
        "default": "SN3D",
        "enum": [
          "SN3D",
          "N3D",
          "FuMa"
        ],
        "type": "string"
      },
      "order": {
        "default": 1,
        "maximum": 7,
        "minimum": 1,
        "type": "integer"
      },
      "source": {
        "type": [
          "integer",
          "string"
        ]
      },
      "spreadDeg": {
        "default": 30,
        "type": "number"
      },
      "suite": {
        "default": "auto",
        "enum": [
          "auto",
          "IEM",
          "SPARTA",
          "ATK",
          "ambiX"
        ],
        "type": "string"
      }
    },
    "required": [
      "source"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channels": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "diff": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "encoder": {
        "type": "string"
      },
      "error": {
        "type": "string"
      },
      "monitor": {
        "type": [
          "object",
          "null"
        ]
      },
      "normalization": {
        "type": "string"
      },
      "normalizationParam": {
        "type": [
          "object",
          "null"
        ]
      },
      "ok": {
        "type": "boolean"
      },
      "order": {
        "type": "integer"
      },
      "orderParam": {
        "type": [
          "object",
          "null"
        ]
      },
      "plan": {
        "type": "array"
      },
      "positions": {
        "type": "array"
      },
      "remediation": {
        "type": "string"
      },
      "sketch": {
        "type": "boolean"
      },
      "track": {
        "type": "integer"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `spatial.stop_head_tracking`

**Profile:** `spatial` · **Hints:** idempotent

Stop the live head-tracking OSC listener started by spatial.add_binaural_monitor: closes the UDP port and stops driving the rotator. Safe to call when nothing is running (returns wasRunning=false). The binaural monitor bus itself is left in place.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `ok`, `packetsReceived`, `wasRunning`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "packetsReceived": {
        "type": "integer"
      },
      "wasRunning": {
        "type": "boolean"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

### Mixing

_Track FX, parameter automation envelopes, faders, and immersive-aware style chains._

#### `envelope.add_point`

**Profile:** `mixing` · **Hints:** mutating

Insert an automation point on a track envelope (looked up by name, e.g. 'Volume', 'Pan'). 'value' is in the envelope's native units (Volume = linear gain, Pan = -1..1). The envelope must already be active on the track; if it isn't, pass autoActivate:true to activate a built-in envelope (Volume/Pan/Width/Mute, their Pre-FX variants, Trim Volume) first.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `autoActivate` | boolean | no | default `false`; activate a built-in track envelope if it is not active yet |
| `envelope` | string | yes | — |
| `selected` | boolean | no | default `false` |
| `shape` | integer | no | default `0`; range [0, 5]; 0 linear,1 square,2 slow start/end,3 fast start,4 fast end,5 bezier |
| `time` | number | yes | — |
| `track` | integer | yes | min 0 |
| `value` | number | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `activated`, `activatedVia`, `envelope`, `ok`, `pointCount`, `time`, `value`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "autoActivate": {
        "default": false,
        "description": "activate a built-in track envelope if it is not active yet",
        "type": "boolean"
      },
      "envelope": {
        "type": "string"
      },
      "selected": {
        "default": false,
        "type": "boolean"
      },
      "shape": {
        "default": 0,
        "description": "0 linear,1 square,2 slow start/end,3 fast start,4 fast end,5 bezier",
        "maximum": 5,
        "minimum": 0,
        "type": "integer"
      },
      "time": {
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "track",
      "envelope",
      "time",
      "value"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "activated": {
        "type": "boolean"
      },
      "activatedVia": {
        "type": "string"
      },
      "envelope": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "pointCount": {
        "type": "integer"
      },
      "time": {
        "type": "number"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `envelope.clear_range`

**Profile:** `mixing` · **Hints:** **destructive**, idempotent

Delete all points of a track envelope inside a [start,end) time range (seconds).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `end` | number | yes | — |
| `envelope` | string | yes | — |
| `start` | number | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `end`, `ok`, `pointCount`, `start`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "end": {
        "type": "number"
      },
      "envelope": {
        "type": "string"
      },
      "start": {
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "envelope",
      "start",
      "end"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "end": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      },
      "pointCount": {
        "type": "integer"
      },
      "start": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `envelope.delete_point`

**Profile:** `mixing` · **Hints:** **destructive**

Delete one point from a track envelope by its index (from envelope.get_points).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `envelope` | string | yes | — |
| `index` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `deletedIndex`, `ok`, `pointCount`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "envelope": {
        "type": "string"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "envelope",
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "deletedIndex": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "pointCount": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "deletedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `envelope.get_points`

**Profile:** `mixing` · **Hints:** read-only, idempotent

List a track envelope's points (time, native-unit value, shape, tension, selected), optionally restricted to a [start,end] time window (seconds).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `end` | number | no | — |
| `envelope` | string | yes | — |
| `start` | number | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `envelope`, `pointCount`, `points`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "end": {
        "type": "number"
      },
      "envelope": {
        "type": "string"
      },
      "start": {
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "envelope"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "envelope": {
        "type": "string"
      },
      "pointCount": {
        "type": "integer"
      },
      "points": {
        "items": {
          "properties": {
            "index": {
              "type": "integer"
            },
            "selected": {
              "type": "boolean"
            },
            "shape": {
              "type": "integer"
            },
            "tension": {
              "type": "number"
            },
            "time": {
              "type": "number"
            },
            "value": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "envelope",
      "pointCount",
      "points"
    ],
    "type": "object"
  }
}
```
</details>

#### `envelope.list`

**Profile:** `mixing` · **Hints:** read-only, idempotent

List a track's active envelopes with index, name, and point count.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `envelopeCount`, `envelopes`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "envelopeCount": {
        "type": "integer"
      },
      "envelopes": {
        "items": {
          "properties": {
            "index": {
              "type": "integer"
            },
            "name": {
              "type": "string"
            },
            "pointCount": {
              "type": "integer"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "envelopeCount",
      "envelopes"
    ],
    "type": "object"
  }
}
```
</details>

#### `envelope.set_automation_mode`

**Profile:** `mixing` · **Hints:** idempotent

Set a track's automation mode (0 trim/read, 1 read, 2 touch, 3 write, 4 latch). Applies to all of the track's envelopes; echoes the previous mode.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `mode` | integer | yes | range [0, 4]; 0 trim/read,1 read,2 touch,3 write,4 latch |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `mode`, `ok`, `previousMode`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "mode": {
        "description": "0 trim/read,1 read,2 touch,3 write,4 latch",
        "maximum": 4,
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "mode"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "mode": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "previousMode": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "mode"
    ],
    "type": "object"
  }
}
```
</details>

#### `fx.add`

**Profile:** `mixing` · **Hints:** mutating

Add an FX by name to a track's FX chain (always instantiates a new instance). Name matches REAPER's Add-FX search (e.g. 'ReaEQ', 'VST3:Pro-Q 3', 'IEM StereoEncoder').

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `name` | string | yes | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxCount`, `fxIndex`, `name`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "name": {
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "name"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxCount": {
        "type": "integer"
      },
      "fxIndex": {
        "type": "integer"
      },
      "name": {
        "type": "string"
      }
    },
    "required": [
      "fxIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `fx.get_param`

**Profile:** `mixing` · **Hints:** read-only, idempotent

Read one FX parameter: raw value + min/max (plugin units) and normalized [0,1].

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `param` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `max`, `min`, `name`, `normalized`, `value`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "param": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "fx",
      "param"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "max": {
        "type": "number"
      },
      "min": {
        "type": "number"
      },
      "name": {
        "type": "string"
      },
      "normalized": {
        "type": "number"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "value",
      "normalized"
    ],
    "type": "object"
  }
}
```
</details>

#### `fx.get_preset`

**Profile:** `mixing` · **Hints:** read-only, idempotent

Read a track FX's current preset: name, 0-based index, and total preset count (index -1 when no named preset is active).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `preset`, `presetCount`, `presetIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "fx"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "preset": {
        "type": "string"
      },
      "presetCount": {
        "type": "integer"
      },
      "presetIndex": {
        "type": "integer"
      }
    },
    "required": [
      "preset",
      "presetIndex",
      "presetCount"
    ],
    "type": "object"
  }
}
```
</details>

#### `fx.list`

**Profile:** `mixing` · **Hints:** read-only, idempotent

List a track's FX chain: index, name, parameter count, enabled state.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fx`, `fxCount`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fx": {
        "items": {
          "properties": {
            "enabled": {
              "type": "boolean"
            },
            "index": {
              "type": "integer"
            },
            "name": {
              "type": "string"
            },
            "numParams": {
              "type": "integer"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "fxCount": {
        "type": "integer"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "fxCount",
      "fx"
    ],
    "type": "object"
  }
}
```
</details>

#### `fx.remove`

**Profile:** `mixing` · **Hints:** **destructive**

Remove an FX from a track's chain by index. Destructive; single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `removedIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "fx"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "removedIndex": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "removedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `fx.set_enabled`

**Profile:** `mixing` · **Hints:** idempotent

Enable or bypass a track FX by index. enabled:false = bypassed (the REAPER 'FX bypass' state). Idempotent; single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `enabled` | boolean | yes | — |
| `fx` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `enabled`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "enabled": {
        "type": "boolean"
      },
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "fx",
      "enabled"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "enabled": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "enabled"
    ],
    "type": "object"
  }
}
```
</details>

#### `fx.set_param`

**Profile:** `mixing` · **Hints:** idempotent

Set one FX parameter. Provide 'normalized' [0,1] OR 'value' (raw plugin units).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `normalized` | number | no | range [0, 1] |
| `param` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |
| `value` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `normalized`, `ok`, `value`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "normalized": {
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "param": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "track",
      "fx",
      "param"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "normalized": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `fx.set_preset`

**Profile:** `mixing` · **Hints:** mutating

Activate a track FX preset. Provide 'preset' (the exact name shown in REAPER's preset dropdown, or a .vstpreset path for VST3) OR 'move' (+N / -N to step through presets). Echoes the resulting preset name.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `move` | integer | no | — |
| `preset` | string | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `preset`, `presetIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "move": {
        "type": "integer"
      },
      "preset": {
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "fx"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "preset": {
        "type": "string"
      },
      "presetIndex": {
        "type": "integer"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `master.add_fx`

**Profile:** `mixing` · **Hints:** mutating

Add an FX by name to the master track's FX chain (always a new instance). Name matches REAPER's Add-FX search. Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `name` | string | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxCount`, `fxIndex`, `name`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "name": {
        "type": "string"
      }
    },
    "required": [
      "name"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxCount": {
        "type": "integer"
      },
      "fxIndex": {
        "type": "integer"
      },
      "name": {
        "type": "string"
      }
    },
    "required": [
      "fxIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `master.get`

**Profile:** `mixing` · **Hints:** read-only, idempotent

Read the master track: volume (dB), mute, channel count, and FX count.

**Parameters**

_No parameters._

**Returns**

Returns a structured object with: `channels`, `fxCount`, `muted`, `volDb`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {},
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channels": {
        "type": "integer"
      },
      "fxCount": {
        "type": "integer"
      },
      "muted": {
        "type": "boolean"
      },
      "volDb": {
        "type": "number"
      }
    },
    "required": [
      "volDb",
      "muted",
      "channels",
      "fxCount"
    ],
    "type": "object"
  }
}
```
</details>

#### `master.remove_fx`

**Profile:** `mixing` · **Hints:** **destructive**

Remove an FX from the master track's FX chain by index (from master.get / fx list order). Completes the master mirror of the track fx.add/fx.remove family.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxCount`, `ok`, `removedIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "fx"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "removedIndex": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "removedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `master.set`

**Profile:** `mixing` · **Hints:** idempotent

Set the master track's volume (dB) and/or mute. Only provided fields change. Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `db` | number | no | — |
| `mute` | boolean | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `muted`, `ok`, `volDb`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "db": {
        "type": "number"
      },
      "mute": {
        "type": "boolean"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "muted": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      },
      "volDb": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `master.set_fx_param`

**Profile:** `mixing` · **Hints:** idempotent

Set one master-FX parameter. Provide 'normalized' [0,1] OR 'value' (raw plugin units). Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `normalized` | number | no | range [0, 1] |
| `param` | integer | yes | min 0 |
| `value` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `normalized`, `ok`, `value`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "normalized": {
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "param": {
        "minimum": 0,
        "type": "integer"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "fx",
      "param"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "normalized": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `mix.apply_style`

**Profile:** `mixing` · **Hints:** idempotent

Insert a NAMED mix/master style chain on a track/bus (pop-bright, cinema-warm, dialog-clear, immersive-master-atmos, warm-master, bright-master). Each step uses the in-box stock FX (ReaEQ/ReaComp/ReaXcomp/ReaLimit — zero dependency) unless a preferred third-party plug-in is detected, then prefers it. Immersive-aware: the limiter ceiling is sized to the target deliverable's true-peak (targetSpec or the style default), and on a bed the LFE is kept OUT of the dynamics (never squashed with the mains). On a WIDE immersive bed (channels > 2) the immersive-master limiter is the bundled multichannel true-peak JSFX (linked gain reduction across ALL mains, LFE-exempt) instead of the stock 2-ch limiter — so the surround/height mains are limited too. One undo block. dryRun returns the exact chain it would insert without mutating.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `ceilingDb` | number | no | — |
| `channels` | integer | no | min 1 |
| `dryRun` | boolean | no | default `false` |
| `style` | string | yes | — |
| `targetSpec` | string | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ceilingDb`, `chain`, `channels`, `detail`, `diff`, `dryRun`, `error`, `hasLfe`, `immersiveAware`, `inserted`, `ok`, `remediation`, `reused`, `stepCount`, `style`, `track`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "ceilingDb": {
        "type": "number"
      },
      "channels": {
        "minimum": 1,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "style": {
        "type": "string"
      },
      "targetSpec": {
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "style"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ceilingDb": {
        "type": "number"
      },
      "chain": {
        "type": "array"
      },
      "channels": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "diff": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "hasLfe": {
        "type": "boolean"
      },
      "immersiveAware": {
        "type": "boolean"
      },
      "inserted": {
        "type": "array"
      },
      "ok": {
        "type": "boolean"
      },
      "remediation": {
        "type": "string"
      },
      "reused": {
        "type": "boolean"
      },
      "stepCount": {
        "type": "integer"
      },
      "style": {
        "type": "string"
      },
      "track": {
        "type": "integer"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `takefx.add`

**Profile:** `mixing` · **Hints:** mutating

Add an FX by name to a take's FX chain (always a new instance). Name matches REAPER's Add-FX search. 'take' defaults to the item's active take.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `name` | string | yes | — |
| `take` | integer | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fxCount`, `fxIndex`, `name`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "name": {
        "type": "string"
      },
      "take": {
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "name"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fxCount": {
        "type": "integer"
      },
      "fxIndex": {
        "type": "integer"
      },
      "name": {
        "type": "string"
      }
    },
    "required": [
      "fxIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `takefx.get_param`

**Profile:** `mixing` · **Hints:** read-only, idempotent

Read one take-FX parameter: raw value + min/max and normalized [0,1].

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `item` | integer | yes | min 0 |
| `param` | integer | yes | min 0 |
| `take` | integer | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `max`, `min`, `name`, `normalized`, `value`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "param": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "fx",
      "param"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "max": {
        "type": "number"
      },
      "min": {
        "type": "number"
      },
      "name": {
        "type": "string"
      },
      "normalized": {
        "type": "number"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "value",
      "normalized"
    ],
    "type": "object"
  }
}
```
</details>

#### `takefx.list`

**Profile:** `mixing` · **Hints:** read-only, idempotent

List a take's FX chain: index, name, parameter count, enabled state.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `take` | integer | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `fx`, `fxCount`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "fx": {
        "items": {
          "properties": {
            "enabled": {
              "type": "boolean"
            },
            "index": {
              "type": "integer"
            },
            "name": {
              "type": "string"
            },
            "numParams": {
              "type": "integer"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "fxCount": {
        "type": "integer"
      }
    },
    "required": [
      "fxCount",
      "fx"
    ],
    "type": "object"
  }
}
```
</details>

#### `takefx.remove`

**Profile:** `mixing` · **Hints:** **destructive**

Remove an FX from a take's chain by index. Destructive; single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `item` | integer | yes | min 0 |
| `take` | integer | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `removedIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "fx"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "removedIndex": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "removedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `takefx.set_enabled`

**Profile:** `mixing` · **Hints:** idempotent

Enable or bypass a take FX by index. Idempotent; single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `enabled` | boolean | yes | — |
| `fx` | integer | yes | min 0 |
| `item` | integer | yes | min 0 |
| `take` | integer | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `enabled`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "enabled": {
        "type": "boolean"
      },
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "fx",
      "enabled"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "enabled": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "enabled"
    ],
    "type": "object"
  }
}
```
</details>

#### `takefx.set_param`

**Profile:** `mixing` · **Hints:** idempotent

Set one take-FX parameter. Provide 'normalized' [0,1] OR 'value' (raw units).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `fx` | integer | yes | min 0 |
| `item` | integer | yes | min 0 |
| `normalized` | number | no | range [0, 1] |
| `param` | integer | yes | min 0 |
| `take` | integer | no | — |
| `track` | integer | yes | min 0 |
| `value` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `normalized`, `ok`, `value`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "fx": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "normalized": {
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "param": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "track",
      "item",
      "fx",
      "param"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "normalized": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      },
      "value": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_fader`

**Profile:** `mixing` · **Hints:** idempotent

Set a track's volume (dB) and/or pan (-1..1).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `db` | number | no | — |
| `pan` | number | no | range [-1, 1] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `db`, `ok`, `pan`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "db": {
        "type": "number"
      },
      "pan": {
        "maximum": 1,
        "minimum": -1,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "db": {
        "type": "number"
      },
      "ok": {
        "type": "boolean"
      },
      "pan": {
        "type": "number"
      }
    },
    "required": [
      "ok"
    ],
    "type": "object"
  }
}
```
</details>

### Routing

_Track-to-track sends and channel-count management._

#### `receive.list`

**Profile:** `routing` · **Hints:** read-only, idempotent

List a track's receives with their SOURCE track, volume (dB), pan, and mute. (send.list with category='receive' reports the same rows keyed differently; this verb resolves the upstream source track explicitly.)

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `receiveCount`, `receives`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "receiveCount": {
        "type": "integer"
      },
      "receives": {
        "items": {
          "properties": {
            "index": {
              "type": "integer"
            },
            "muted": {
              "type": "boolean"
            },
            "pan": {
              "type": "number"
            },
            "srcTrack": {
              "type": "integer"
            },
            "volDb": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "receiveCount",
      "receives"
    ],
    "type": "object"
  }
}
```
</details>

#### `send.add`

**Profile:** `routing` · **Hints:** mutating

Create a track send from a source track to a destination track, with optional initial volume (dB) and pan.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `db` | number | no | — |
| `dest` | integer | yes | min 0 |
| `pan` | number | no | range [-1, 1] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `dest`, `sendIndex`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "db": {
        "type": "number"
      },
      "dest": {
        "minimum": 0,
        "type": "integer"
      },
      "pan": {
        "maximum": 1,
        "minimum": -1,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "dest"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "dest": {
        "type": "integer"
      },
      "sendIndex": {
        "type": "integer"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "sendIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `send.add_hardware_output`

**Profile:** `routing` · **Hints:** mutating

Add a hardware-output send from a track to a physical output channel. 'outputChannel' is the 0-based hardware channel offset (pass a stereo pair's low channel; set 'mono' for a single channel). Optional initial volume (dB) and pan. Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `db` | number | no | — |
| `mono` | boolean | no | default `false` |
| `outputChannel` | integer | yes | min 0 |
| `pan` | number | no | range [-1, 1] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `outputChannel`, `sendIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "db": {
        "type": "number"
      },
      "mono": {
        "default": false,
        "type": "boolean"
      },
      "outputChannel": {
        "minimum": 0,
        "type": "integer"
      },
      "pan": {
        "maximum": 1,
        "minimum": -1,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "outputChannel"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "outputChannel": {
        "type": "integer"
      },
      "sendIndex": {
        "type": "integer"
      }
    },
    "required": [
      "sendIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `send.list`

**Profile:** `routing` · **Hints:** read-only, idempotent

List a track's sends (default), receives, or hardware outputs with destination, volume (dB), pan, mute.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `category` | enum | no | one of: `send`, `receive`, `hwout`; default `"send"` |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `category`, `sendCount`, `sends`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "category": {
        "default": "send",
        "enum": [
          "send",
          "receive",
          "hwout"
        ],
        "type": "string"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "category": {
        "type": "string"
      },
      "sendCount": {
        "type": "integer"
      },
      "sends": {
        "items": {
          "properties": {
            "destTrack": {
              "type": "integer"
            },
            "index": {
              "type": "integer"
            },
            "muted": {
              "type": "boolean"
            },
            "pan": {
              "type": "number"
            },
            "volDb": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "sendCount",
      "sends"
    ],
    "type": "object"
  }
}
```
</details>

#### `send.remove`

**Profile:** `routing` · **Hints:** **destructive**

Remove a send/receive/hardware-output by its index (from send.list).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `category` | enum | no | one of: `send`, `receive`, `hwout`; default `"send"` |
| `index` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ok`, `removedIndex`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "category": {
        "default": "send",
        "enum": [
          "send",
          "receive",
          "hwout"
        ],
        "type": "string"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ok": {
        "type": "boolean"
      },
      "removedIndex": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "removedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `send.set`

**Profile:** `routing` · **Hints:** idempotent

Set parameters of an existing send/receive/hardware-output (by index from send.list / receive.list): volume (dB), pan (-1..1), mute, phase, and send mode (0=post-fader, 1=pre-fx, 3=post-fx). Only the fields you provide are changed. Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `category` | enum | no | one of: `send`, `receive`, `hwout`; default `"send"` |
| `db` | number | no | — |
| `index` | integer | yes | min 0 |
| `mode` | enum | no | one of: `0`, `1`, `3` |
| `mute` | boolean | no | — |
| `pan` | number | no | range [-1, 1] |
| `phase` | boolean | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `index`, `mode`, `muted`, `ok`, `pan`, `volDb`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "category": {
        "default": "send",
        "enum": [
          "send",
          "receive",
          "hwout"
        ],
        "type": "string"
      },
      "db": {
        "type": "number"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "mode": {
        "enum": [
          0,
          1,
          3
        ],
        "type": "integer"
      },
      "mute": {
        "type": "boolean"
      },
      "pan": {
        "maximum": 1,
        "minimum": -1,
        "type": "number"
      },
      "phase": {
        "type": "boolean"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "index": {
        "type": "integer"
      },
      "mode": {
        "type": "integer"
      },
      "muted": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      },
      "pan": {
        "type": "number"
      },
      "volDb": {
        "type": "number"
      }
    },
    "required": [
      "ok",
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `send.set_channels`

**Profile:** `routing` · **Hints:** idempotent

Set a send's source and/or destination channel routing (I_SRCCHAN / I_DSTCHAN). 'srcChannel' and 'dstChannel' are 0-based channel offsets on the source and destination tracks. 'srcChannels' is the send width (1=mono, 2=stereo (default), 4, 6, …). srcChannel=-1 sends no audio (MIDI-only). This is the channel-level routing that beds/bus wiring depend on — keep offsets consistent with the immersive layout (e.g. a 7.1.4 bed's height pair). Single-undo.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `category` | enum | no | one of: `send`, `receive`, `hwout`; default `"send"` |
| `dstChannel` | integer | no | min 0 |
| `dstMono` | boolean | no | default `false` |
| `index` | integer | yes | min 0 |
| `srcChannel` | integer | no | min -1 |
| `srcChannels` | enum | no | one of: `1`, `2`, `4`, `6`, `8`, `10`, `12`, `14`, `16` |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `dstChan`, `index`, `ok`, `srcChan`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "category": {
        "default": "send",
        "enum": [
          "send",
          "receive",
          "hwout"
        ],
        "type": "string"
      },
      "dstChannel": {
        "minimum": 0,
        "type": "integer"
      },
      "dstMono": {
        "default": false,
        "type": "boolean"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "srcChannel": {
        "minimum": -1,
        "type": "integer"
      },
      "srcChannels": {
        "enum": [
          1,
          2,
          4,
          6,
          8,
          10,
          12,
          14,
          16
        ],
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "dstChan": {
        "type": "integer"
      },
      "index": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "srcChan": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `track.set_channels`

**Profile:** `routing` · **Hints:** idempotent

Set a track's channel count (2..64; REAPER uses even counts). 12 = 7.1.4, 16 = 3rd-order ambisonics, 24 = 9.1.6, etc. The multichannel foundation for immersive routing.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `channels` | integer | yes | range [2, 64] |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `channels`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "channels": {
        "maximum": 64,
        "minimum": 2,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "channels"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channels": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "channels"
    ],
    "type": "object"
  }
}
```
</details>

### MIDI

_Takes and MIDI note / CC CRUD._

#### `midi.apply_groove`

**Profile:** `midi` · **Hints:** mutating

Apply a groove template to notes: per-grid-slot timing offsets + velocity scaling. Provide a 'template' array of {timeOffset (beats), velScale} slots (e.g. from midi.extract_groove), or a named 'groove' (swing8 \| swing16) with an 'amount' 0..1. strength 0..1 scales the applied feel.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `amount` | number | no | default `0.33`; range [0, 1] |
| `grid` | number | no | default `0.25` |
| `groove` | enum | no | one of: `swing8`, `swing16` |
| `item` | integer | yes | min 0 |
| `selectedOnly` | boolean | no | default `false` |
| `strength` | number | no | default `1`; range [0, 1] |
| `take` | integer | no | default `-1`; min -1 |
| `template` | array&lt;object&gt; | no | — |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `changed`, `noteCount`, `ok`, `slots`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "amount": {
        "default": 0.33,
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "grid": {
        "default": 0.25,
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "groove": {
        "enum": [
          "swing8",
          "swing16"
        ],
        "type": "string"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "strength": {
        "default": 1,
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "template": {
        "items": {
          "properties": {
            "timeOffset": {
              "type": "number"
            },
            "velScale": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "changed": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "slots": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "changed"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.create_item`

**Profile:** `midi` · **Hints:** mutating

Create a new empty MIDI item (with one active MIDI take) on a track, at a position and length in seconds. Returns the item's index for use with the midi.* / take.* tools.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `length` | number | no | — |
| `name` | string | no | — |
| `position` | number | no | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `guid`, `item`, `length`, `position`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "length": {
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "name": {
        "type": "string"
      },
      "position": {
        "minimum": 0,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "guid": {
        "type": "string"
      },
      "item": {
        "type": "integer"
      },
      "length": {
        "type": "number"
      },
      "position": {
        "type": "number"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "position",
      "length"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.delete_cc`

**Profile:** `midi` · **Hints:** **destructive**

Delete a CC/other event from a MIDI take by its index (from midi.list_cc).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `index` | integer | yes | min 0 |
| `item` | integer | yes | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ccCount`, `deletedIndex`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ccCount": {
        "type": "integer"
      },
      "deletedIndex": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "deletedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.delete_note`

**Profile:** `midi` · **Hints:** **destructive**

Delete a note from a MIDI take by its index (from midi.list_notes).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `index` | integer | yes | min 0 |
| `item` | integer | yes | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `deletedIndex`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "deletedIndex": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "deletedIndex"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.extract_groove`

**Profile:** `midi` · **Hints:** read-only, idempotent

Analyze a MIDI take and return a groove template: per-grid-slot mean timing deviation (beats) and mean velocity ratio (vs refVelocity). Feed the returned 'template' to midi.apply_groove to stamp this feel onto another take. Read-only.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `grid` | number | no | default `0.25` |
| `item` | integer | yes | min 0 |
| `refVelocity` | integer | no | default `96`; range [1, 127] |
| `selectedOnly` | boolean | no | default `false` |
| `steps` | integer | no | default `2`; range [1, 64] |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `grid`, `item`, `noteCount`, `steps`, `template`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "grid": {
        "default": 0.25,
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "refVelocity": {
        "default": 96,
        "maximum": 127,
        "minimum": 1,
        "type": "integer"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "steps": {
        "default": 2,
        "maximum": 64,
        "minimum": 1,
        "type": "integer"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "grid": {
        "type": "number"
      },
      "item": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "steps": {
        "type": "integer"
      },
      "template": {
        "items": {
          "properties": {
            "timeOffset": {
              "type": "number"
            },
            "velScale": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "grid",
      "steps",
      "template"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.humanize`

**Profile:** `midi` · **Hints:** mutating

Randomly nudge note timing (+/- timing beats) and velocity (+/- velocity) for a human feel. Deterministic given 'seed' (same seed -> same result). selectedOnly limits to selected notes.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `seed` | integer | no | default `1`; min 0 |
| `selectedOnly` | boolean | no | default `false` |
| `take` | integer | no | default `-1`; min -1 |
| `timing` | number | no | default `0.02`; min 0 |
| `track` | integer | yes | min 0 |
| `velocity` | integer | no | default `8`; min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `changed`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "seed": {
        "default": 1,
        "minimum": 0,
        "type": "integer"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "timing": {
        "default": 0.02,
        "minimum": 0,
        "type": "number"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "velocity": {
        "default": 8,
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "changed": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "changed"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.insert_cc`

**Profile:** `midi` · **Hints:** mutating

Insert a MIDI CC / program / channel-pressure / pitch-bend event into a take at a quarter-note beat position (relative to item start). messageType default 'cc': set 'cc' (controller 0..127) and 'value' 0..127. For 'pitch', value is 0..16383 (8192 = center).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `beats` | number | yes | min 0 |
| `cc` | integer | no | default `1`; range [0, 127] |
| `channel` | integer | no | default `0`; range [0, 15] |
| `item` | integer | yes | min 0 |
| `messageType` | enum | no | one of: `cc`, `program`, `channel_pressure`, `pitch`, `poly_pressure`; default `"cc"` |
| `muted` | boolean | no | default `false` |
| `selected` | boolean | no | default `false` |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |
| `value` | integer | no | default `0`; range [0, 16383] |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `cc`, `ccCount`, `channel`, `messageType`, `ok`, `value`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "beats": {
        "minimum": 0,
        "type": "number"
      },
      "cc": {
        "default": 1,
        "maximum": 127,
        "minimum": 0,
        "type": "integer"
      },
      "channel": {
        "default": 0,
        "maximum": 15,
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "messageType": {
        "default": "cc",
        "enum": [
          "cc",
          "program",
          "channel_pressure",
          "pitch",
          "poly_pressure"
        ],
        "type": "string"
      },
      "muted": {
        "default": false,
        "type": "boolean"
      },
      "selected": {
        "default": false,
        "type": "boolean"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "value": {
        "default": 0,
        "maximum": 16383,
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "beats"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "cc": {
        "type": "integer"
      },
      "ccCount": {
        "type": "integer"
      },
      "channel": {
        "type": "integer"
      },
      "messageType": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "value": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "messageType",
      "ccCount"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.insert_note`

**Profile:** `midi` · **Hints:** mutating

Insert a MIDI note into a take. Timing is in quarter-note beats relative to the item start (startBeats, lengthBeats). pitch 0..127 (60 = middle C), velocity 1..127, channel 0..15.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `channel` | integer | no | default `0`; range [0, 15] |
| `item` | integer | yes | min 0 |
| `lengthBeats` | number | no | — |
| `muted` | boolean | no | default `false` |
| `pitch` | integer | yes | range [0, 127] |
| `selected` | boolean | no | default `false` |
| `startBeats` | number | yes | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |
| `velocity` | integer | no | default `96`; range [1, 127] |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `channel`, `lengthBeats`, `noteCount`, `ok`, `pitch`, `startBeats`, `velocity`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "channel": {
        "default": 0,
        "maximum": 15,
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "lengthBeats": {
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "muted": {
        "default": false,
        "type": "boolean"
      },
      "pitch": {
        "maximum": 127,
        "minimum": 0,
        "type": "integer"
      },
      "selected": {
        "default": false,
        "type": "boolean"
      },
      "startBeats": {
        "minimum": 0,
        "type": "number"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "velocity": {
        "default": 96,
        "maximum": 127,
        "minimum": 1,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "startBeats",
      "pitch"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channel": {
        "type": "integer"
      },
      "lengthBeats": {
        "type": "number"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "pitch": {
        "type": "integer"
      },
      "startBeats": {
        "type": "number"
      },
      "velocity": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "pitch",
      "startBeats",
      "noteCount"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.insert_notes`

**Profile:** `midi` · **Hints:** mutating

Insert a batch of MIDI notes into a take in one undo block (deferred sort — much faster than repeated midi.insert_note for chords/phrases). Each note: startBeats + pitch required; lengthBeats default 1, velocity default 96, channel default 0.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `notes` | array&lt;object&gt; | yes | — |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `inserted`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "notes": {
        "items": {
          "properties": {
            "channel": {
              "default": 0,
              "maximum": 15,
              "minimum": 0,
              "type": "integer"
            },
            "lengthBeats": {
              "default": 1,
              "exclusiveMinimum": 0,
              "type": "number"
            },
            "muted": {
              "default": false,
              "type": "boolean"
            },
            "pitch": {
              "maximum": 127,
              "minimum": 0,
              "type": "integer"
            },
            "selected": {
              "default": false,
              "type": "boolean"
            },
            "startBeats": {
              "minimum": 0,
              "type": "number"
            },
            "velocity": {
              "default": 96,
              "maximum": 127,
              "minimum": 1,
              "type": "integer"
            }
          },
          "required": [
            "startBeats",
            "pitch"
          ],
          "type": "object"
        },
        "maxItems": 4096,
        "minItems": 1,
        "type": "array"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "notes"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "inserted": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "inserted"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.legato`

**Profile:** `midi` · **Hints:** mutating

Stretch each note's length so it reaches the next note's start (minus 'gap' beats), floored at 'minLength'. Notes are processed in time order; the last note keeps its length. selectedOnly treats only the selected notes as the sequence.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `gap` | number | no | default `0`; min 0 |
| `item` | integer | yes | min 0 |
| `minLength` | number | no | default `0.03125` |
| `selectedOnly` | boolean | no | default `false` |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `changed`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "gap": {
        "default": 0,
        "minimum": 0,
        "type": "number"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "minLength": {
        "default": 0.03125,
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "changed": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "changed"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.list_cc`

**Profile:** `midi` · **Hints:** read-only, idempotent

List the CC / program / pitch / pressure events in a MIDI take.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ccCount`, `events`, `item`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ccCount": {
        "type": "integer"
      },
      "events": {
        "items": {
          "properties": {
            "beats": {
              "type": "number"
            },
            "channel": {
              "type": "integer"
            },
            "index": {
              "type": "integer"
            },
            "messageType": {
              "type": "string"
            },
            "msg2": {
              "type": "integer"
            },
            "msg3": {
              "type": "integer"
            },
            "value": {
              "type": "integer"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "item": {
        "type": "integer"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "ccCount",
      "events"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.list_notes`

**Profile:** `midi` · **Hints:** read-only, idempotent

List the notes in a MIDI take (default: active take), with beats, pitch, velocity, channel and flags.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `item`, `noteCount`, `notes`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "item": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "notes": {
        "items": {
          "properties": {
            "channel": {
              "type": "integer"
            },
            "index": {
              "type": "integer"
            },
            "lengthBeats": {
              "type": "number"
            },
            "muted": {
              "type": "boolean"
            },
            "pitch": {
              "type": "integer"
            },
            "selected": {
              "type": "boolean"
            },
            "startBeats": {
              "type": "number"
            },
            "startTime": {
              "type": "number"
            },
            "velocity": {
              "type": "integer"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "noteCount",
      "notes"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.nudge`

**Profile:** `midi` · **Hints:** mutating

Shift note start positions by 'beats' (can be negative; starts clamp at 0), keeping length. selectedOnly limits to selected notes.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `beats` | number | yes | — |
| `item` | integer | yes | min 0 |
| `selectedOnly` | boolean | no | default `false` |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `moved`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "beats": {
        "type": "number"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "beats"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "moved": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "moved"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.quantize`

**Profile:** `midi` · **Hints:** mutating

Quantize note start positions to a grid (beats). strength 0..1 (1 = full snap), swing 0..1 pushes off-beat grid slots later. quantizeEnds also snaps note ends; otherwise length is kept. selectedOnly limits the edit to selected notes.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `grid` | number | no | default `0.25` |
| `item` | integer | yes | min 0 |
| `quantizeEnds` | boolean | no | default `false` |
| `selectedOnly` | boolean | no | default `false` |
| `strength` | number | no | default `1`; range [0, 1] |
| `swing` | number | no | default `0`; range [0, 1] |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `changed`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "grid": {
        "default": 0.25,
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "quantizeEnds": {
        "default": false,
        "type": "boolean"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "strength": {
        "default": 1,
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "swing": {
        "default": 0,
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "changed": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "changed"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.scale_velocity`

**Profile:** `midi` · **Hints:** mutating

Scale note velocities around a center: v' = center + (v-center)*scale + offset, clamped to 1..127. scale>1 expands dynamics, <1 compresses. selectedOnly limits to selected notes.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `center` | integer | no | default `64`; range [0, 127] |
| `item` | integer | yes | min 0 |
| `offset` | number | no | default `0` |
| `scale` | number | no | default `1`; min 0 |
| `selectedOnly` | boolean | no | default `false` |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `changed`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "center": {
        "default": 64,
        "maximum": 127,
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "offset": {
        "default": 0,
        "type": "number"
      },
      "scale": {
        "default": 1,
        "minimum": 0,
        "type": "number"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "changed": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "changed"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.select_notes`

**Profile:** `midi` · **Hints:** idempotent

Select or deselect notes in a MIDI take. With no filters, applies to ALL MIDI content (fast path). Filters: pitchMin/pitchMax, channel, startBeats/endBeats window.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `channel` | integer | no | range [0, 15] |
| `endBeats` | number | no | min 0 |
| `item` | integer | yes | min 0 |
| `pitchMax` | integer | no | range [0, 127] |
| `pitchMin` | integer | no | range [0, 127] |
| `selected` | boolean | no | default `true` |
| `startBeats` | number | no | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `matched`, `ok`, `selected`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "channel": {
        "maximum": 15,
        "minimum": 0,
        "type": "integer"
      },
      "endBeats": {
        "minimum": 0,
        "type": "number"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "pitchMax": {
        "maximum": 127,
        "minimum": 0,
        "type": "integer"
      },
      "pitchMin": {
        "maximum": 127,
        "minimum": 0,
        "type": "integer"
      },
      "selected": {
        "default": true,
        "type": "boolean"
      },
      "startBeats": {
        "minimum": 0,
        "type": "number"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "matched": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "selected": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "selected"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.set_cc`

**Profile:** `midi` · **Hints:** idempotent

Edit a CC/program/pressure/pitch event in place by its index (from midi.list_cc). Only provided fields change (beats/channel/cc/value/selected/muted). 'value' is decoded per the event's message type (pitch = 14-bit 0..16383, cc = controller value, else the data byte).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `beats` | number | no | min 0 |
| `cc` | integer | no | range [0, 127] |
| `channel` | integer | no | range [0, 15] |
| `index` | integer | yes | min 0 |
| `item` | integer | yes | min 0 |
| `muted` | boolean | no | — |
| `selected` | boolean | no | — |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |
| `value` | integer | no | range [0, 16383] |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `ccCount`, `index`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "beats": {
        "minimum": 0,
        "type": "number"
      },
      "cc": {
        "maximum": 127,
        "minimum": 0,
        "type": "integer"
      },
      "channel": {
        "maximum": 15,
        "minimum": 0,
        "type": "integer"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "muted": {
        "type": "boolean"
      },
      "selected": {
        "type": "boolean"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "value": {
        "maximum": 16383,
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "ccCount": {
        "type": "integer"
      },
      "index": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.set_note`

**Profile:** `midi` · **Hints:** idempotent

Edit a MIDI note in place by its index (from midi.list_notes). Only provided fields change (startBeats/lengthBeats/pitch/velocity/channel/selected/muted). Note indices are re-sorted by time after an edit — re-list before further index-addressed edits.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `channel` | integer | no | range [0, 15] |
| `index` | integer | yes | min 0 |
| `item` | integer | yes | min 0 |
| `lengthBeats` | number | no | — |
| `muted` | boolean | no | — |
| `pitch` | integer | no | range [0, 127] |
| `selected` | boolean | no | — |
| `startBeats` | number | no | min 0 |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |
| `velocity` | integer | no | range [1, 127] |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `index`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "channel": {
        "maximum": 15,
        "minimum": 0,
        "type": "integer"
      },
      "index": {
        "minimum": 0,
        "type": "integer"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "lengthBeats": {
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "muted": {
        "type": "boolean"
      },
      "pitch": {
        "maximum": 127,
        "minimum": 0,
        "type": "integer"
      },
      "selected": {
        "type": "boolean"
      },
      "startBeats": {
        "minimum": 0,
        "type": "number"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      },
      "velocity": {
        "maximum": 127,
        "minimum": 1,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "index"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "index": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "index"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.stretch`

**Profile:** `midi` · **Hints:** mutating

Time-scale note positions and lengths around an anchor beat: pos' = anchor + (pos-anchor)*factor, length' = length*factor. factor>1 slows/spreads, <1 tightens. selectedOnly limits to selected notes.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `anchorBeats` | number | no | default `0`; min 0 |
| `factor` | number | yes | — |
| `item` | integer | yes | min 0 |
| `selectedOnly` | boolean | no | default `false` |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `changed`, `noteCount`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "anchorBeats": {
        "default": 0,
        "minimum": 0,
        "type": "number"
      },
      "factor": {
        "exclusiveMinimum": 0,
        "type": "number"
      },
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "factor"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "changed": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "changed"
    ],
    "type": "object"
  }
}
```
</details>

#### `midi.transpose`

**Profile:** `midi` · **Hints:** mutating

Transpose notes by a number of semitones (can be negative). Notes that would leave 0..127 are left unchanged and counted as skipped. selectedOnly limits to selected notes.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `selectedOnly` | boolean | no | default `false` |
| `semitones` | integer | yes | — |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `moved`, `noteCount`, `ok`, `skipped`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "selectedOnly": {
        "default": false,
        "type": "boolean"
      },
      "semitones": {
        "type": "integer"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "semitones"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "moved": {
        "type": "integer"
      },
      "noteCount": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "skipped": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "moved"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.list`

**Profile:** `midi` · **Hints:** read-only, idempotent

List an item's takes with index, name, whether active, and whether MIDI.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `activeTake`, `item`, `takeCount`, `takes`, `track`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "activeTake": {
        "type": "integer"
      },
      "item": {
        "type": "integer"
      },
      "takeCount": {
        "type": "integer"
      },
      "takes": {
        "items": {
          "properties": {
            "active": {
              "type": "boolean"
            },
            "index": {
              "type": "integer"
            },
            "isMidi": {
              "type": "boolean"
            },
            "name": {
              "type": "string"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "track": {
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "takeCount",
      "activeTake",
      "takes"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.set_active`

**Profile:** `midi` · **Hints:** idempotent

Set which take of an item is active, by take index.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `take` | integer | yes | min 0 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `activeTake`, `ok`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "take": {
        "minimum": 0,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "take"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "activeTake": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      }
    },
    "required": [
      "ok",
      "activeTake"
    ],
    "type": "object"
  }
}
```
</details>

#### `take.set_name`

**Profile:** `midi` · **Hints:** idempotent

Set a take's name (default: the active take).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `item` | integer | yes | min 0 |
| `name` | string | yes | — |
| `take` | integer | no | default `-1`; min -1 |
| `track` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `name`, `ok`, `take`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "item": {
        "minimum": 0,
        "type": "integer"
      },
      "name": {
        "type": "string"
      },
      "take": {
        "default": -1,
        "minimum": -1,
        "type": "integer"
      },
      "track": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "track",
      "item",
      "name"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "name": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "take": {
        "type": "integer"
      }
    },
    "required": [
      "ok",
      "name"
    ],
    "type": "object"
  }
}
```
</details>

### Render

_Multichannel / immersive deliverable rendering._

#### `project.render`

**Profile:** `render` · **Hints:** mutating

Render the project to disk using its already-configured render settings (format, channels, sources) — a plain, non-immersive render distinct from spatial.render_deliverables. Optionally override the output directory ('path'), file-name 'pattern', render bounds ('boundsFlag' + 'startPos'/'endPos'), and 'addToProject'. dryRun (the DEFAULT) reports the exact output files the current settings would write WITHOUT rendering; set dryRun:false to actually render (this blocks the main thread for the render's duration — bound long projects with startPos/endPos). All overridden RENDER_* settings are snapshotted and restored.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `addToProject` | boolean | no | — |
| `boundsFlag` | integer | no | range [0, 7] |
| `dryRun` | boolean | no | default `true` |
| `endPos` | number | no | — |
| `path` | string | no | — |
| `pattern` | string | no | — |
| `startPos` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `dryRun`, `ok`, `stats`, `targets`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "addToProject": {
        "type": "boolean"
      },
      "boundsFlag": {
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": true,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "path": {
        "type": "string"
      },
      "pattern": {
        "type": "string"
      },
      "startPos": {
        "type": "number"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "dryRun": {
        "type": "boolean"
      },
      "ok": {
        "type": "boolean"
      },
      "stats": {
        "type": "string"
      },
      "targets": {
        "items": {
          "type": "string"
        },
        "type": "array"
      },
      "warnings": {
        "items": {
          "type": "string"
        },
        "type": "array"
      }
    },
    "required": [
      "ok",
      "dryRun"
    ],
    "type": "object"
  }
}
```
</details>

#### `spatial.export_adm`

**Profile:** `render` · **Hints:** mutating

Author a native ITU-R BS.2076 ADM object-audio deliverable — a Broadcast-Wave (RIFF/WAVE, or BW64/RF64 per BS.2088 when >4 GiB) carrying a chna (channel allocation) + axml (the ADM XML) chunk beside the interleaved PCM. This is a capability REAPER lacks natively (see spatial.render_deliverables' atmos-send-layout, which can only wire the topology to an EXTERNAL Dolby Renderer). The deliverable = a DirectSpeakers BED (bedTrack at bedLayout, e.g. Atmos 7.1.2) plus N mono OBJECTS (objectTracks), each object carrying a position/gain TRAJECTORY (audioBlockFormat time-slices) SAMPLED from its panner automation over the render window (spherical az/el/distance by default; coordinateMode:cartesian emits X/Y/Z). Bed + object stems are rendered bit-exact (RENDER_* snapshot/restore, temps deleted); each object's mono essence = its rendered channel 1, its position = read from the object panner's azimuth/elevation/distance params (best-effort, like analysis.object_loudness). dryRun (the DEFAULT is false) samples the positions + plans the ADM model WITHOUT rendering or writing. Bound long programmes with boundsFlag=0 + startPos/endPos (a render blocks the main thread). Ingest-verify the file against your certified renderer (Dolby/EBU/Nuendo); inspect it with analysis.adm_inspect. Attach per-object BS.2076 metadata via objectMetadata[] (each entry keyed by its object 'track'): extent — a 'size' 0..1 knob (sets width=height=depth; the ADM normalized object size, natural in cartesian mode) or explicit width/height/depth (spherical: width/height in DEGREES, depth 0..1); objectDivergence — 'divergence' 0..1 (split into two mirror copies) with an optional 'divergenceRange' (azimuthRange deg in spherical / positionRange 0..1 in cartesian; defaults 45°/0.5); and 'importance' 0..10 (a renderer may drop lower-importance objects under load). Objects with no entry stay point sources with importance omitted. Set profile:"dolby-atmos" to author a Dolby Atmos Master ADM Profile v1.0-conformant file: it forces cartesian objects (X/Y/Z clamped [-1,1]), collapses extent to an identical [0,1] size, STRIPS objectDivergence (prohibited) and importance (permitted only on inactive objects), and emits the interpolationLength ramp (0 then 250 samples); it FAILS CLOSED on a non-Atmos bed (7.1.4/9.1.6/22.2 — route those channels as objects), a non-48k render, or >118 objects / >128 channels. The applied normalizations are echoed in 'profile'. Validate any ADM BWF's conformance with analysis.adm_profile_check. (Profile-shaped + self-validated; certified-renderer ingest is your check.)

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bedLayout` | enum | no | one of: `5.1`, `7.1`, `7.1.2`, `7.1.4`, `9.1.6`, `22.2`; default `"7.1.2"` |
| `bedTrack` | integer | no | min 0 |
| `bitDepth` | enum | no | one of: `16`, `24`, `32`; default `24` |
| `blockMs` | number | no | default `100`; min 1 |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `contentName` | string | no | — |
| `coordinateMode` | enum | no | one of: `spherical`, `cartesian`; default `"spherical"` |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `maxBlocks` | integer | no | default `1000`; min 1 |
| `objectMetadata` | array&lt;object&gt; | no | — |
| `objectTracks` | array&lt;integer&gt; | no | — |
| `outPath` | string | no | — |
| `profile` | enum | no | one of: `none`, `dolby-atmos`; default `"none"` |
| `programmeName` | string | no | — |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `adm`, `bedChannels`, `bedLayout`, `bitDepth`, `channels`, `container`, `coordinateMode`, `detail`, `dryRun`, `durationSec`, `error`, `frames`, `note`, `objectCount`, `objectMetadataCount`, `objects`, `ok`, `path`, `profile`, `remediation`, `sampleRate`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bedLayout": {
        "default": "7.1.2",
        "enum": [
          "5.1",
          "7.1",
          "7.1.2",
          "7.1.4",
          "9.1.6",
          "22.2"
        ],
        "type": "string"
      },
      "bedTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "bitDepth": {
        "default": 24,
        "enum": [
          16,
          24,
          32
        ],
        "type": "integer"
      },
      "blockMs": {
        "default": 100,
        "minimum": 1,
        "type": "number"
      },
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "contentName": {
        "type": "string"
      },
      "coordinateMode": {
        "default": "spherical",
        "enum": [
          "spherical",
          "cartesian"
        ],
        "type": "string"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "maxBlocks": {
        "default": 1000,
        "minimum": 1,
        "type": "integer"
      },
      "objectMetadata": {
        "items": {
          "additionalProperties": false,
          "properties": {
            "depth": {
              "minimum": 0,
              "type": "number"
            },
            "divergence": {
              "maximum": 1,
              "minimum": 0,
              "type": "number"
            },
            "divergenceRange": {
              "minimum": 0,
              "type": "number"
            },
            "height": {
              "minimum": 0,
              "type": "number"
            },
            "importance": {
              "maximum": 10,
              "minimum": 0,
              "type": "integer"
            },
            "size": {
              "maximum": 1,
              "minimum": 0,
              "type": "number"
            },
            "track": {
              "minimum": 0,
              "type": "integer"
            },
            "width": {
              "minimum": 0,
              "type": "number"
            }
          },
          "required": [
            "track"
          ],
          "type": "object"
        },
        "type": "array"
      },
      "objectTracks": {
        "items": {
          "minimum": 0,
          "type": "integer"
        },
        "type": "array"
      },
      "outPath": {
        "type": "string"
      },
      "profile": {
        "default": "none",
        "enum": [
          "none",
          "dolby-atmos"
        ],
        "type": "string"
      },
      "programmeName": {
        "type": "string"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "adm": {
        "type": "object"
      },
      "bedChannels": {
        "type": "integer"
      },
      "bedLayout": {
        "type": "string"
      },
      "bitDepth": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "container": {
        "type": "string"
      },
      "coordinateMode": {
        "type": "string"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "durationSec": {
        "type": "number"
      },
      "error": {
        "type": "string"
      },
      "frames": {
        "type": "integer"
      },
      "note": {
        "type": "string"
      },
      "objectCount": {
        "type": "integer"
      },
      "objectMetadataCount": {
        "type": "integer"
      },
      "objects": {
        "type": "array"
      },
      "ok": {
        "type": "boolean"
      },
      "path": {
        "type": "string"
      },
      "profile": {
        "type": "object"
      },
      "remediation": {
        "type": "string"
      },
      "sampleRate": {
        "type": "number"
      },
      "warnings": {
        "items": {
          "type": "string"
        },
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `spatial.export_damf`

**Profile:** `render` · **Hints:** mutating

Author a native Dolby Atmos Master File (DAMF) — the Dolby Atmos Renderer's triad master, the sibling of ADM BWF (spatial.export_adm): three same-basename files, <base>.atmos (YAML manifest — version, presentation, the file references, and the bed+object roster each with a 1-based input ID), <base>.atmos.metadata (YAML — sampleRate + a flat list of position 'events' keyed by ID: bed channels are static, objects carry one event per trajectory block {ID, samplePos, active, pos:[x,y,z]}), and <base>.atmos.audio (a big-endian Core Audio Format / CAF PCM file — the interleaved essence, BED CHANNELS FIRST then OBJECTS in roster order). Same inputs as spatial.export_adm: a DirectSpeakers BED (bedTrack at bedLayout, <= Atmos 7.1.2) plus N mono OBJECTS (objectTracks), each object's position/gain TRAJECTORY SAMPLED from its panner automation over the render window; bed + object stems are rendered bit-exact (RENDER_* snapshot/restore, temps deleted). DAMF is inherently Dolby Atmos, so the Master ADM Profile v1.0 constraints are ENFORCED (FAILS CLOSED on a bed above 7.1.2 — route heights/wides as objects — a non-48k render, or >118 objects / >128 channels); positions are the DAMF room frame x[-1..1] L..R, y[-1..1] back..front, z[0..1] floor..ceiling. Attach a per-object extent via objectMetadata[] (each keyed by its object 'track'): 'size' 0..1 (the Atmos object size). outPath is a BASENAME (any trailing .atmos/.atmos.audio/.atmos.metadata is stripped); the three files are written beside it. dryRun (DEFAULT false) samples positions + plans the triad WITHOUT rendering or writing. Bound long programmes with boundsFlag=0 + startPos/endPos (a render blocks the main thread). Inspect the result with analysis.damf_inspect; ingest-verify against your certified Dolby Atmos Renderer / Conversion Tool. (DAMF-shaped + self-validated; certified-renderer ingest is your check.)

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bedLayout` | enum | no | one of: `5.1`, `7.1`, `7.1.2`; default `"7.1.2"` |
| `bedTrack` | integer | no | min 0 |
| `bitDepth` | enum | no | one of: `16`, `24`, `32`; default `24` |
| `blockMs` | number | no | default `100`; min 1 |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `contentName` | string | no | — |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `maxBlocks` | integer | no | default `1000`; min 1 |
| `objectMetadata` | array&lt;object&gt; | no | — |
| `objectTracks` | array&lt;integer&gt; | no | — |
| `outPath` | string | no | — |
| `programmeName` | string | no | — |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `basePath`, `bedChannels`, `bedLayout`, `bitDepth`, `channels`, `damf`, `detail`, `dryRun`, `durationSec`, `error`, `files`, `frames`, `note`, `objectCount`, `objectMetadataCount`, `objects`, `ok`, `path`, `remediation`, `sampleRate`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bedLayout": {
        "default": "7.1.2",
        "enum": [
          "5.1",
          "7.1",
          "7.1.2"
        ],
        "type": "string"
      },
      "bedTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "bitDepth": {
        "default": 24,
        "enum": [
          16,
          24,
          32
        ],
        "type": "integer"
      },
      "blockMs": {
        "default": 100,
        "minimum": 1,
        "type": "number"
      },
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "contentName": {
        "type": "string"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "maxBlocks": {
        "default": 1000,
        "minimum": 1,
        "type": "integer"
      },
      "objectMetadata": {
        "items": {
          "additionalProperties": false,
          "properties": {
            "size": {
              "maximum": 1,
              "minimum": 0,
              "type": "number"
            },
            "track": {
              "minimum": 0,
              "type": "integer"
            }
          },
          "required": [
            "track"
          ],
          "type": "object"
        },
        "type": "array"
      },
      "objectTracks": {
        "items": {
          "minimum": 0,
          "type": "integer"
        },
        "type": "array"
      },
      "outPath": {
        "type": "string"
      },
      "programmeName": {
        "type": "string"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "basePath": {
        "type": "string"
      },
      "bedChannels": {
        "type": "integer"
      },
      "bedLayout": {
        "type": "string"
      },
      "bitDepth": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "damf": {
        "type": "object"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "durationSec": {
        "type": "number"
      },
      "error": {
        "type": "string"
      },
      "files": {
        "type": "object"
      },
      "frames": {
        "type": "integer"
      },
      "note": {
        "type": "string"
      },
      "objectCount": {
        "type": "integer"
      },
      "objectMetadataCount": {
        "type": "integer"
      },
      "objects": {
        "type": "array"
      },
      "ok": {
        "type": "boolean"
      },
      "path": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "sampleRate": {
        "type": "number"
      },
      "warnings": {
        "items": {
          "type": "string"
        },
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `spatial.render_deliverables`

**Profile:** `render` · **Hints:** mutating

Render immersive deliverables via REAPER's render engine (RENDER_* project settings + a no-dialog render action): in-box multichannel bed WAV, ambiX (ACN/SN3D) B-format WAV, and binaural downmix masters, each with REAPER-native integrated-LUFS + true-peak from RENDER_STATS (cross-checked against ffmpeg ebur128 in the verify harness). Also builds the bed+object send layout into the external Dolby Atmos Renderer / DAPS (REAPER cannot author ADM BWF natively). Targets: multichannel \| ambix \| binaural \| atmos-send-layout. Point each render target at its bus via bedTrack/ambixTrack/binauralTrack (or sourceTrack; omit = master mix). dryRun reports the exact files RENDER_TARGETS would write without rendering; for the real render leg, bound it with boundsFlag=0 + startPos/endPos so it fits the tools/call window. The tool snapshots and restores all RENDER_* settings + track selection.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `ambixTrack` | integer | no | min 0 |
| `bedLayout` | enum | no | one of: `5.1`, `7.1`, `7.1.4`, `9.1.6`, `22.2` |
| `bedTrack` | integer | no | min 0 |
| `binauralTrack` | integer | no | min 0 |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `format` | string | no | — |
| `measureLoudness` | boolean | no | default `true` |
| `objectTracks` | array&lt;integer&gt; | no | — |
| `outDir` | string | no | — |
| `pattern` | string | no | — |
| `renderAction` | integer | no | default `41824` |
| `rendererTrack` | integer | no | min 0 |
| `sampleRate` | integer | no | default `0`; min 0 |
| `sourceTrack` | integer | no | min 0 |
| `startPos` | number | no | — |
| `targets` | array&lt;string&gt; | yes | — |

**Returns**

Returns a structured object with: `atmos`, `dryRun`, `format`, `formatSource`, `rendered`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "properties": {
      "ambixTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "bedLayout": {
        "enum": [
          "5.1",
          "7.1",
          "7.1.4",
          "9.1.6",
          "22.2"
        ],
        "type": "string"
      },
      "bedTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "binauralTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "format": {
        "type": "string"
      },
      "measureLoudness": {
        "default": true,
        "type": "boolean"
      },
      "objectTracks": {
        "items": {
          "minimum": 0,
          "type": "integer"
        },
        "type": "array"
      },
      "outDir": {
        "type": "string"
      },
      "pattern": {
        "type": "string"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "rendererTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "sampleRate": {
        "default": 0,
        "minimum": 0,
        "type": "integer"
      },
      "sourceTrack": {
        "minimum": 0,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "targets": {
        "items": {
          "enum": [
            "multichannel",
            "ambix",
            "binaural",
            "atmos-send-layout"
          ],
          "type": "string"
        },
        "minItems": 1,
        "type": "array"
      }
    },
    "required": [
      "targets"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "atmos": {
        "type": "object"
      },
      "dryRun": {
        "type": "boolean"
      },
      "format": {
        "type": "string"
      },
      "formatSource": {
        "type": "string"
      },
      "rendered": {
        "items": {
          "properties": {
            "ambisonicOrder": {
              "type": "integer"
            },
            "channels": {
              "type": "integer"
            },
            "dryRun": {
              "type": "boolean"
            },
            "loudnessEngine": {
              "type": "string"
            },
            "lra": {
              "type": "number"
            },
            "lufs": {
              "type": "number"
            },
            "path": {
              "type": "string"
            },
            "plannedTargets": {
              "items": {
                "type": "string"
              },
              "type": "array"
            },
            "rawStats": {
              "type": "string"
            },
            "samplePeak": {
              "type": "number"
            },
            "source": {
              "type": "string"
            },
            "statsSummary": {
              "type": "string"
            },
            "target": {
              "type": "string"
            },
            "truePeak": {
              "type": "number"
            }
          },
          "type": "object"
        },
        "type": "array"
      },
      "warnings": {
        "items": {
          "type": "string"
        },
        "type": "array"
      }
    },
    "required": [
      "rendered"
    ],
    "type": "object"
  }
}
```
</details>

### Analysis

_Deliverable-spec conformance (loudness + true-peak, per-bed)._

#### `analysis.accessor_meter`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Full per-channel meter of a track's item content or a take's source, render-free, via a REAPER audio accessor (READ-ONLY). Reports per-channel RMS / sample peak / oversampled true-peak (dBTP) / K-weighted level with SMPTE bed-layout labels (LFE flagged), L/R phase correlation, and BS.1770-4 GATED loudness (integrated + ungated + activity fraction) computed in-box from the samples — no RENDER_STATS, no render round-trip. A TRACK target measures the summed item/take content BEFORE track FX/volume/pan; add itemIndex (+ optional takeIndex) for one take. The fast, frame-accurate accessor sibling of analysis.meter — use analysis.meter for the post-FX bus/master (it renders and adds REAPER-native program loudness).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `dryRun` | boolean | no | default `false` |
| `duration` | number | no | — |
| `itemIndex` | integer | no | — |
| `sampleRate` | integer | no | — |
| `start` | number | no | default `0` |
| `takeIndex` | integer | no | — |
| `target` | integer \| string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `channels`, `channelsDetail`, `clamped`, `detail`, `downmix`, `dryRun`, `duration`, `error`, `frames`, `layout`, `loudness`, `measuredSource`, `plan`, `remediation`, `sampleRate`, `silent`, `source`, `sourceExtent`, `target`, `warnings`, `window`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "duration": {
        "type": "number"
      },
      "itemIndex": {
        "type": "integer"
      },
      "sampleRate": {
        "type": "integer"
      },
      "start": {
        "default": 0,
        "type": "number"
      },
      "takeIndex": {
        "type": "integer"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channels": {
        "type": "integer"
      },
      "channelsDetail": {
        "type": "array"
      },
      "clamped": {
        "type": "boolean"
      },
      "detail": {
        "type": "string"
      },
      "downmix": {
        "type": "object"
      },
      "dryRun": {
        "type": "boolean"
      },
      "duration": {
        "type": "number"
      },
      "error": {
        "type": "string"
      },
      "frames": {
        "type": "number"
      },
      "layout": {
        "type": "string"
      },
      "loudness": {
        "type": "object"
      },
      "measuredSource": {
        "type": "string"
      },
      "plan": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "sampleRate": {
        "type": "integer"
      },
      "silent": {
        "type": "boolean"
      },
      "source": {
        "type": "string"
      },
      "sourceExtent": {
        "type": "object"
      },
      "target": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      },
      "window": {
        "type": "object"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.adm_inspect`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Parse + summarize an ADM (ITU-R BS.2076) Broadcast-Wave deliverable (READ-ONLY). Read the file at 'path', decode its chna (channel-allocation) + axml (ADM XML) chunks, and report: the container (RIFF/WAVE vs BW64/RF64), the PCM format (channels / sampleRate / bitDepth / frames / duration), the chna track table (audioTrackUID + track/pack refs per data channel), and an ADM structure summary (audioObject / audioPackFormat / audioChannelFormat / audioBlockFormat counts, coordinate mode spherical\|cartesian, DirectSpeakers/Objects presence, object names, programme name, BS.2076 version, and the per-object metadata footprint — extent / objectDivergence / importance presence + block counts + the importance values). Round-trips spatial.export_adm and QCs ANY third-party ADM BWF (Dolby / Nuendo / EBU). This is a structural summarizer, not an XSD validator — it flags missing or inconsistent chunks (e.g. chna numTracks != fmt channels). 'path' may be absolute or relative to the project directory. SDK-free — the parser is host-unit-tested (unit.adm).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `path` | string | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `adm`, `chna`, `chunks`, `container`, `detail`, `error`, `format`, `isAdm`, `path`, `remediation`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "path": {
        "type": "string"
      }
    },
    "required": [
      "path"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "adm": {
        "type": [
          "object",
          "null"
        ]
      },
      "chna": {
        "type": [
          "object",
          "null"
        ]
      },
      "chunks": {
        "type": "array"
      },
      "container": {
        "type": "string"
      },
      "detail": {
        "type": "string"
      },
      "error": {
        "type": "string"
      },
      "format": {
        "type": "object"
      },
      "isAdm": {
        "type": "boolean"
      },
      "path": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "warnings": {
        "items": {
          "type": "string"
        },
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.adm_profile_check`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Check an ADM (ITU-R BS.2076) Broadcast-Wave against the Dolby Atmos Master ADM Profile v1.0 (READ-ONLY) — the constraints that make an ADM BWF ingestable by a certified Dolby renderer AS an Atmos master (Dolby's Conversion Tool round-trips a conformant file to/from a DAMF). Reads the file at 'path', parses its chna+axml (like analysis.adm_inspect), and reports 'conformant' plus a list of per-rule 'violations' (each a code + detail): sample_rate (must be 48000); bed_layout / channel_cap (<=128) / object_cap (<=118); programme_id (must be APR_1001); coordinate (objects must be cartesian); extent (width=depth=height in [0,1]); divergence (objectDivergence is PROHIBITED); importance (only on inactive objects); interpolation (the 0 then 250-sample ramp). Point it at ANY ADM BWF — spatial.export_adm's profile:"dolby-atmos" output, or a third-party (Nuendo / Pro Tools / Dolby) file — to QC it before delivery. 'path' may be absolute or relative to the project directory. SDK-free — the validator is host-unit-tested (unit.adm_profile). Structural conformance to the published profile, NOT a guarantee a specific renderer ingests the file (that is the ingest check on your tools).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `path` | string | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `adm`, `conformant`, `detail`, `error`, `format`, `isAdm`, `noteCount`, `notes`, `path`, `profile`, `remediation`, `violations`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "path": {
        "type": "string"
      }
    },
    "required": [
      "path"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "adm": {
        "type": [
          "object",
          "null"
        ]
      },
      "conformant": {
        "type": "boolean"
      },
      "detail": {
        "type": "string"
      },
      "error": {
        "type": "string"
      },
      "format": {
        "type": "object"
      },
      "isAdm": {
        "type": "boolean"
      },
      "noteCount": {
        "type": "integer"
      },
      "notes": {
        "type": "array"
      },
      "path": {
        "type": "string"
      },
      "profile": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "violations": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.binaural_check`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Binaural / headphone-deliverable QC for a 2-channel binaural monitor bus or stereo binaural render (READ-ONLY) — the headphone counterpart to analysis.downmix_check's loudspeaker fold. Point it at the spatial.add_binaural_monitor bus (or any 2-ch binaural render). Runs ONE bounded measure-don't-limit render (RENDER_STATS program loudness + true peak; snapshots + restores every RENDER_* + selection; temp deleted), then computes from the samples the binaural-specific metrics: L/R phase CORRELATION (image integrity / mono compatibility), the INTER-AURAL LEVEL BALANCE (L vs R K-weighted level — a lopsided or collapsed HRTF image), and the MID/SIDE ratio (near-mono collapse vs healthy width), plus per-ear RMS/peak/true-peak (headphone clip risk). Accepts a 2-ch stereo bus OR a wider binaural monitor bus (the add_binaural_monitor bus is sized to receive the ambisonic send with the decoded binaural on channels 1/2 — those two channels are measured); fails closed on a labeled loudspeaker BED (use analysis.downmix_check) or a mono target. Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |
| `target` | integer \| string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `boundsFlag`, `channels`, `correlation`, `detail`, `dryRun`, `error`, `interAuralBalanceDb`, `interpretation`, `left`, `measuredSource`, `midSide`, `plan`, `program`, `remediation`, `right`, `target`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "boundsFlag": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "correlation": {
        "type": "number"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "interAuralBalanceDb": {
        "type": "number"
      },
      "interpretation": {
        "type": "string"
      },
      "left": {
        "type": "object"
      },
      "measuredSource": {
        "type": "string"
      },
      "midSide": {
        "type": "object"
      },
      "plan": {
        "type": "string"
      },
      "program": {
        "type": "object"
      },
      "remediation": {
        "type": "string"
      },
      "right": {
        "type": "object"
      },
      "target": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.check_deliverable`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Measure a master (or a specified bus/track) and report pass/fail against a NAMED deliverable spec — integrated LUFS, short-term max, and true-peak (dBTP) — one of the named deliverable specs (atmos-music, streaming-stereo, apple-music-stereo, ebu-r128, ebu-r128-s1, atsc-a85, podcast, cinema-theatrical). Measurement uses REAPER's native RENDER_STATS engine (ITU-R BS.1770-5 K-weighted LUFS + inter-sample true peak; cross-check ffmpeg ebur128 in verify) and is READ-ONLY: it snapshots and restores every RENDER_* setting + selection, reporting without shipping. Pass measured:{lufsIntegrated,truePeak,shortTermMax} to evaluate numbers directly (no render); otherwise it runs one bounded analysis render — bound it with boundsFlag=0 + startPos/endPos to fit the call window. cinema-theatrical is SPL-referenced, so LUFS is reported but not gated (true-peak still is). PER-BED: pass beds:[trackIdx\|name,…] (or perBed:true to auto-detect bed-width buses = 6/8/12/16/24 ch) to render each bed bus as its own stem in ONE pass and report per-bed pass/fail + a rollup{pass,worstLufs,worstTruePeak,failing}. In supplied mode pass measured as an ARRAY of measurement blocks for the same per-bed report (no render).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `beds` | array&lt;['integer', 'string']&gt; | no | — |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `measured` | object \| array | no | — |
| `outDir` | string | no | — |
| `perBed` | boolean | no | default `false` |
| `renderAction` | integer | no | default `41824` |
| `spec` | string | yes | — |
| `startPos` | number | no | — |
| `target` | integer \| string | no | — |
| `toleranceLU` | number | no | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `beds`, `channels`, `checks`, `detail`, `dryRun`, `error`, `layout`, `lufsGated`, `measuredSource`, `metrics`, `pass`, `perBed`, `rawStats`, `remediation`, `rollup`, `spec`, `standardsBasis`, `target`, `toleranceNote`, `use`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "beds": {
        "items": {
          "type": [
            "integer",
            "string"
          ]
        },
        "type": "array"
      },
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "measured": {
        "type": [
          "object",
          "array"
        ]
      },
      "outDir": {
        "type": "string"
      },
      "perBed": {
        "default": false,
        "type": "boolean"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "spec": {
        "type": "string"
      },
      "startPos": {
        "type": "number"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      },
      "toleranceLU": {
        "minimum": 0,
        "type": "number"
      }
    },
    "required": [
      "spec"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "beds": {
        "type": "array"
      },
      "channels": {
        "type": "integer"
      },
      "checks": {
        "type": "array"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "layout": {
        "type": "string"
      },
      "lufsGated": {
        "type": "boolean"
      },
      "measuredSource": {
        "type": "string"
      },
      "metrics": {
        "type": "object"
      },
      "pass": {
        "type": [
          "boolean",
          "null"
        ]
      },
      "perBed": {
        "type": "array"
      },
      "rawStats": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "rollup": {
        "type": "object"
      },
      "spec": {
        "type": "string"
      },
      "standardsBasis": {
        "type": "string"
      },
      "target": {
        "type": "string"
      },
      "toleranceNote": {
        "type": "string"
      },
      "use": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.damf_inspect`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Parse + summarize a Dolby Atmos Master File (DAMF) triad (READ-ONLY) — the Dolby Atmos Renderer's native master, the sibling of ADM BWF (see spatial.export_damf). Point 'path' at the '.atmos' manifest (or either sibling — the base is derived); the tool reads all three files and reports: the manifest roster (bedInstances / bed channel count / object count), the metadata (sampleRate + position-event count), and the CAF essence (format / channels / bitDepth / sampleRate / frames / duration / endianness). It warns when the CAF channel count disagrees with the bed+object roster or the essence is little-endian (Dolby masters are big-endian PCM). Round-trips spatial.export_damf and QCs a third-party triad (Pro Tools / Nuendo / DaVinci / Dolby Conversion Tool). 'path' may be absolute or relative to the project directory. SDK-free — host-unit-tested (unit.damf). A structural summarizer, NOT a certified-renderer ingest guarantee.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `path` | string | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `audio`, `detail`, `error`, `isDamf`, `manifest`, `metadata`, `path`, `remediation`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "path": {
        "type": "string"
      }
    },
    "required": [
      "path"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "audio": {
        "type": "object"
      },
      "detail": {
        "type": "string"
      },
      "error": {
        "type": "string"
      },
      "isDamf": {
        "type": "boolean"
      },
      "manifest": {
        "type": "object"
      },
      "metadata": {
        "type": "object"
      },
      "path": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "warnings": {
        "items": {
          "type": "string"
        },
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.decode_coverage`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Decode-coverage metering of an ACN/SN3D ambisonic scene bus (READ-ONLY) — the spatial complement of analysis.spatial_field's single DoA: instead of 'where is the dominant source' it answers 'how EVENLY is the whole sphere covered — are there directional holes, is the mix front-heavy or missing height'. Runs ONE bounded BIT-EXACT render (snapshots + restores every RENDER_* + selection; temp deleted), decodes the scene onto a virtual-speaker set with the real SN3D spherical harmonics (higher order => sharper), and reports the per-direction ENERGY distribution: a UNIFORMITY index (normalized entropy, 1 = perfectly even), coefficient of variation, the energy CONCENTRATION in the loudest ~10% of directions, the count of DEAD ZONES (directions below deadZoneFrac of the mean), the front/back · left/right · upper/lower · ear-level energy balance, the dominant direction, and the top energy hotspots. Default decode is a uniform Fibonacci sphere of `speakers` points; pass layout ('5.1'/'7.1'/'7.1.4'/'9.1.6') to decode to that delivery array's nominal directions and get per-speaker energy instead. Order inferred from channel count ((order+1)²); pass normalization='n3d' if the bus is N3D. Fails closed if the target is not a valid ambisonic width (≥4 ch). Bound with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `deadZoneFrac` | number | no | default `0.1`; range [0, 1] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `layout` | enum | no | one of: `5.1`, `7.1`, `7.1.4`, `9.1.6` |
| `normalization` | enum | no | one of: `sn3d`, `n3d`; default `"sn3d"` |
| `order` | integer | no | min 1 |
| `renderAction` | integer | no | default `41824` |
| `speakers` | integer | no | default `64`; range [12, 512] |
| `startPos` | number | no | — |
| `target` | integer \| string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `acnUsed`, `balance`, `boundsFlag`, `channels`, `coefficientOfVariation`, `concentrationTop`, `deadZones`, `detail`, `dominant`, `dryRun`, `error`, `hotspots`, `interpretation`, `measuredSource`, `mode`, `normalization`, `order`, `perSpeaker`, `plan`, `remediation`, `speakers`, `target`, `uniformity`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "deadZoneFrac": {
        "default": 0.1,
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "layout": {
        "enum": [
          "5.1",
          "7.1",
          "7.1.4",
          "9.1.6"
        ],
        "type": "string"
      },
      "normalization": {
        "default": "sn3d",
        "enum": [
          "sn3d",
          "n3d"
        ],
        "type": "string"
      },
      "order": {
        "minimum": 1,
        "type": "integer"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "speakers": {
        "default": 64,
        "maximum": 512,
        "minimum": 12,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "acnUsed": {
        "type": "integer"
      },
      "balance": {
        "type": "object"
      },
      "boundsFlag": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "coefficientOfVariation": {
        "type": "number"
      },
      "concentrationTop": {
        "type": "number"
      },
      "deadZones": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "dominant": {
        "type": "object"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "hotspots": {
        "type": "array"
      },
      "interpretation": {
        "type": "string"
      },
      "measuredSource": {
        "type": "string"
      },
      "mode": {
        "type": "string"
      },
      "normalization": {
        "type": "string"
      },
      "order": {
        "type": "integer"
      },
      "perSpeaker": {
        "type": [
          "array",
          "null"
        ]
      },
      "plan": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "speakers": {
        "type": "integer"
      },
      "target": {
        "type": "string"
      },
      "uniformity": {
        "type": "number"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.detect_silence`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Frame-accurate silence detection on a track's item content or a take's source, render-free, via a REAPER audio accessor (READ-ONLY). Reads a bounded window (no render) and reports leading and trailing silence, internal silent gaps, and the content-bearing span for trimming — useful for finding dead air, top/tail points, and gaps between phrases. A region is silent where the trailing-window cross-channel RMS stays below thresholdDb for at least minSilenceSec; windowMs smooths the detector. A TRACK target reads item content pre track-FX; add itemIndex (+ optional takeIndex) for one take. Times are in the accessor's time base (project time for a track, source time for a take).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `dryRun` | boolean | no | default `false` |
| `duration` | number | no | — |
| `itemIndex` | integer | no | — |
| `minSilenceSec` | number | no | default `0.3` |
| `sampleRate` | integer | no | — |
| `start` | number | no | default `0` |
| `takeIndex` | integer | no | — |
| `target` | integer \| string | no | — |
| `thresholdDb` | number | no | default `-60` |
| `windowMs` | number | no | default `10` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `clamped`, `contentSpan`, `detail`, `dryRun`, `duration`, `error`, `frames`, `fullySilent`, `internalGaps`, `leadingSilenceSec`, `measuredSource`, `minSilenceSec`, `plan`, `remediation`, `sampleRate`, `silent`, `source`, `sourceExtent`, `target`, `thresholdDb`, `trailingSilenceSec`, `warnings`, `window`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "duration": {
        "type": "number"
      },
      "itemIndex": {
        "type": "integer"
      },
      "minSilenceSec": {
        "default": 0.3,
        "type": "number"
      },
      "sampleRate": {
        "type": "integer"
      },
      "start": {
        "default": 0,
        "type": "number"
      },
      "takeIndex": {
        "type": "integer"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      },
      "thresholdDb": {
        "default": -60,
        "type": "number"
      },
      "windowMs": {
        "default": 10,
        "type": "number"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "clamped": {
        "type": "boolean"
      },
      "contentSpan": {
        "type": "object"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "duration": {
        "type": "number"
      },
      "error": {
        "type": "string"
      },
      "frames": {
        "type": "number"
      },
      "fullySilent": {
        "type": "boolean"
      },
      "internalGaps": {
        "type": "array"
      },
      "leadingSilenceSec": {
        "type": "number"
      },
      "measuredSource": {
        "type": "string"
      },
      "minSilenceSec": {
        "type": "number"
      },
      "plan": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "sampleRate": {
        "type": "integer"
      },
      "silent": {
        "type": "boolean"
      },
      "source": {
        "type": "string"
      },
      "sourceExtent": {
        "type": "object"
      },
      "target": {
        "type": "string"
      },
      "thresholdDb": {
        "type": "number"
      },
      "trailingSilenceSec": {
        "type": "number"
      },
      "warnings": {
        "type": "array"
      },
      "window": {
        "type": "object"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.dialog_loudness`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Dialog loudness QC (READ-ONLY): designate the DIALOG track/bus and get (1) program loudness of the mix from REAPER's native RENDER_STATS, (2) the dialog stem's SPEECH-ACTIVE loudness — ITU-R BS.1770-4 gated loudness computed in C++ on a bit-exact render of the ISOLATED dialog stem, where the -70 LKFS absolute + -10 LU relative gates drop the silence between phrases (an honest approximation of dialogue-gated measurement — valid when the stem carries dialog only; NOT Dolby Dialogue Intelligence VAD), (3) the dialog activity fraction, and (4) the program-to-dialog offset (dialnorm-style; e.g. Netflix specifies dialogue-gated -27 LKFS ±2 LU). Runs TWO bounded renders (program + dialog stem), each snapshotting + restoring every RENDER_* field + selection, temp files deleted. Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection) and keep it short — renders are synchronous.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dialogTrack` | integer \| string | yes | — |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |
| `target` | integer \| string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `boundsFlag`, `detail`, `dialog`, `dialogLabel`, `dialogTrack`, `dryRun`, `error`, `interpretation`, `measuredSource`, `method`, `plan`, `program`, `programToDialogLu`, `remediation`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dialogTrack": {
        "type": [
          "integer",
          "string"
        ]
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      }
    },
    "required": [
      "dialogTrack"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "boundsFlag": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "dialog": {
        "type": "object"
      },
      "dialogLabel": {
        "type": "string"
      },
      "dialogTrack": {
        "type": "integer"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "interpretation": {
        "type": "string"
      },
      "measuredSource": {
        "type": "string"
      },
      "method": {
        "type": "string"
      },
      "plan": {
        "type": "string"
      },
      "program": {
        "type": "object"
      },
      "programToDialogLu": {
        "type": [
          "number",
          "null"
        ]
      },
      "remediation": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.downmix_check`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Downmix compatibility QC for a multichannel master/bus (READ-ONLY). Runs ONE bounded, BIT-EXACT render (snapshot + restore every RENDER_* + selection; temp deleted), then folds the channels in C++ per ITU-R BS.775 practice — L/R at 0 dB, C at -3 dB to both, surrounds/heights/wides to their side at -3 dB, LFE omitted (includeLfe:true folds it at -3 dB) — and measures what the fold does: stereo downmix BS.1770-4 gated loudness + true peak + Lo/Ro correlation, MONO fold ((Lo+Ro)/2) loudness + true peak, and the level deltas (multichannel→stereo, stereo→mono). A mono delta well below -3 LU beyond the decorrelation baseline flags phase cancellation. Supported widths: 2 (stereo mono-check), 5.1, 7.1, 7.1.4, 9.1.6; the fold matrix is echoed per channel. Loudness here is the C++ BS.1770-4 gated engine (unit-tested host-side) because the folds exist only in memory — program loudness of the ORIGINAL mix still belongs to analysis.meter/RENDER_STATS.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `includeLfe` | boolean | no | default `false` |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |
| `target` | integer \| string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `boundsFlag`, `channels`, `coefficients`, `deltas`, `detail`, `dryRun`, `error`, `includeLfe`, `interpretation`, `layout`, `measuredSource`, `mono`, `multichannel`, `plan`, `remediation`, `stereo`, `target`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "includeLfe": {
        "default": false,
        "type": "boolean"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "boundsFlag": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "coefficients": {
        "type": "array"
      },
      "deltas": {
        "type": "object"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "includeLfe": {
        "type": "boolean"
      },
      "interpretation": {
        "type": "string"
      },
      "layout": {
        "type": "string"
      },
      "measuredSource": {
        "type": "string"
      },
      "mono": {
        "type": "object"
      },
      "multichannel": {
        "type": "object"
      },
      "plan": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "stereo": {
        "type": "object"
      },
      "target": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.loudness_timeline`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Loudness-over-time for a master/bus (READ-ONLY) — the data behind a loudness graph. Runs ONE bounded measure-don't-limit render (RENDER_STATS program block included for reference; snapshot + restore; temp deleted), then computes in C++ from the samples: MOMENTARY (400 ms) and SHORT-TERM (3 s) BS.1770-4 K-weighted series on a common hop (default 0.1 s; channel weights from the bed labels, LFE excluded), the maxima WITH their positions in time, and the EBU Tech 3342 loudness-range detail (gated 10th/95th percentiles). The series is capped at maxPoints (default 1000) by auto-coarsening the hop — a long selection still returns a readable curve. Times are seconds from the START of the analyzed range. Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `hopSec` | number | no | default `0.1`; range [0.02, 2] |
| `maxPoints` | integer | no | default `1000`; range [50, 4000] |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |
| `target` | integer \| string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `boundsFlag`, `channels`, `detail`, `dryRun`, `error`, `hopSec`, `layout`, `lra`, `maxMomentary`, `maxShortTerm`, `measuredSource`, `momentary`, `plan`, `points`, `program`, `remediation`, `shortTerm`, `target`, `times`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "hopSec": {
        "default": 0.1,
        "maximum": 2,
        "minimum": 0.02,
        "type": "number"
      },
      "maxPoints": {
        "default": 1000,
        "maximum": 4000,
        "minimum": 50,
        "type": "integer"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "boundsFlag": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "hopSec": {
        "type": "number"
      },
      "layout": {
        "type": "string"
      },
      "lra": {
        "type": "object"
      },
      "maxMomentary": {
        "type": "object"
      },
      "maxShortTerm": {
        "type": "object"
      },
      "measuredSource": {
        "type": "string"
      },
      "momentary": {
        "type": "array"
      },
      "plan": {
        "type": "string"
      },
      "points": {
        "type": "integer"
      },
      "program": {
        "type": "object"
      },
      "remediation": {
        "type": "string"
      },
      "shortTerm": {
        "type": "array"
      },
      "target": {
        "type": "string"
      },
      "times": {
        "type": "array"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.meter`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Full multichannel meter for a master or bus (READ-ONLY). Reports PROGRAM loudness from REAPER's native RENDER_STATS (integrated LUFS, short-term/momentary max, loudness range, true-peak, sample peak — ITU-R BS.1770) AND per-channel metrics the render engine cannot give: per-channel RMS, sample peak, oversampled true-peak (dBTP) and K-weighted level, with SMPTE bed-layout labels (5.1/7.1/7.1.4/9.1.6/22.2) and LFE flagged. Also reports L/R phase correlation (mono/downmix compatibility). Runs ONE bounded, non-destructive analysis render (snapshots + restores every RENDER_* field + selection; the temp file is deleted after reading) using the measure-don't-limit config, so a headroomed master is measured exactly. Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection) to fit the call window and avoid a long synchronous render. Complements analysis.check_deliverable (spec pass/fail) and analysis.spatial_field (ambisonic direction).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |
| `target` | integer \| string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `boundsFlag`, `channels`, `channelsDetail`, `detail`, `downmix`, `dryRun`, `error`, `layout`, `measuredSource`, `plan`, `program`, `rawStats`, `remediation`, `target`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "boundsFlag": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "channelsDetail": {
        "type": "array"
      },
      "detail": {
        "type": "string"
      },
      "downmix": {
        "type": "object"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "layout": {
        "type": "string"
      },
      "measuredSource": {
        "type": "string"
      },
      "plan": {
        "type": "string"
      },
      "program": {
        "type": "object"
      },
      "rawStats": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "target": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.object_decode_timeline`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Per-object decode-coverage TIMELINE (READ-ONLY) — the object-model complement of analysis.decode_coverage (which decodes a whole ambisonic SCENE bus): for each immersive OBJECT it answers, over time, 'which delivery loudspeaker(s) does this object energize, and how does that migrate as it moves?'. PURELY GEOMETRIC — it reads each object's position TRAJECTORY (no render, no audio): either from a live object track's panner automation (azimuth/elevation/distance, sampled over the window exactly like spatial.export_adm) via `objects`, or from explicit `trajectories` keyframes. At each time slice it ENCODES the object direction to real SN3D spherical harmonics at `order` and PROJECTION-DECODES onto the chosen layout's nominal speaker directions (the same basis analysis.decode_coverage uses — sharper at higher order), picking the dominant speaker. Per object it reports the timeline (dominant speaker + position per slice), the dominant-speaker MIGRATION path, per-speaker DWELL fractions, the great-circle angular TRAVEL + elevation reach, and a time-weighted coverage FOOTPRINT (hemisphere balance, uniformity, dead zones, speakers touched). Azimuth is +LEFT/CCW (the ambisonic/IEM convention, matching ambisonic_encode/export_adm); distance is carried for reporting only (direction is distance-invariant). Give `objects` (live tracks — omit both to auto-discover 'Object N' tracks) and/or `trajectories` (explicit). Bound live sampling with boundsFlag (0=custom startPos/endPos, 1=ENTIRE PROJECT — the default, 2=time selection), blockMs, and maxBlocks.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `blockMs` | number | no | default `100`; min 1 |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `deadZoneFrac` | number | no | default `0.1`; range [0, 1] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `includePerSpeaker` | boolean | no | default `false` |
| `layout` | enum | no | one of: `5.1`, `7.1`, `7.1.4`, `9.1.6`; default `"7.1.4"` |
| `maxBlocks` | integer | no | default `1000`; range [1, 20000] |
| `maxTimelinePoints` | integer | no | default `48`; range [2, 512] |
| `objects` | array&lt;['integer', 'string']&gt; | no | — |
| `order` | integer | no | default `3`; range [1, 7] |
| `startPos` | number | no | — |
| `trajectories` | array&lt;object&gt; | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `boundsFlag`, `count`, `detail`, `dryRun`, `error`, `interpretation`, `layout`, `measuredSource`, `objects`, `order`, `remediation`, `speakers`, `warnings`, `window`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "blockMs": {
        "default": 100,
        "minimum": 1,
        "type": "number"
      },
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "deadZoneFrac": {
        "default": 0.1,
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "includePerSpeaker": {
        "default": false,
        "type": "boolean"
      },
      "layout": {
        "default": "7.1.4",
        "enum": [
          "5.1",
          "7.1",
          "7.1.4",
          "9.1.6"
        ],
        "type": "string"
      },
      "maxBlocks": {
        "default": 1000,
        "maximum": 20000,
        "minimum": 1,
        "type": "integer"
      },
      "maxTimelinePoints": {
        "default": 48,
        "maximum": 512,
        "minimum": 2,
        "type": "integer"
      },
      "objects": {
        "items": {
          "type": [
            "integer",
            "string"
          ]
        },
        "maxItems": 128,
        "type": "array"
      },
      "order": {
        "default": 3,
        "maximum": 7,
        "minimum": 1,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "trajectories": {
        "items": {
          "additionalProperties": false,
          "properties": {
            "gain": {
              "type": "number"
            },
            "keyframes": {
              "items": {
                "additionalProperties": false,
                "properties": {
                  "azimuthDeg": {
                    "type": "number"
                  },
                  "distance": {
                    "type": "number"
                  },
                  "elevationDeg": {
                    "type": "number"
                  },
                  "gain": {
                    "type": "number"
                  },
                  "t": {
                    "type": "number"
                  }
                },
                "type": "object"
              },
              "minItems": 1,
              "type": "array"
            },
            "name": {
              "type": "string"
            }
          },
          "required": [
            "keyframes"
          ],
          "type": "object"
        },
        "type": "array"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "boundsFlag": {
        "type": "integer"
      },
      "count": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "interpretation": {
        "type": "string"
      },
      "layout": {
        "type": "string"
      },
      "measuredSource": {
        "type": "string"
      },
      "objects": {
        "type": "array"
      },
      "order": {
        "type": "integer"
      },
      "remediation": {
        "type": "string"
      },
      "speakers": {
        "type": "array"
      },
      "warnings": {
        "type": "array"
      },
      "window": {
        "type": [
          "object",
          "null"
        ]
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.object_loudness`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Per-object loudness + position for a LIST of Atmos/immersive OBJECT tracks in ONE bounded render pass (READ-ONLY) — the object-model balance readout (objects are usually mono streams with a panner, unlike the layout beds analysis.stem_loudness targets). Renders the given object tracks as BIT-EXACT stems (no normalize/limit; snapshots + restores every RENDER_* + selection; temps deleted), then per object computes in C++: ITU-R BS.1770-4 GATED integrated loudness, ungated level, activity fraction, oversampled true peak (dBTP) and sample peak, each object's SHARE of the total object energy, and its offset from the LOUDEST object (deltaFromLoudestLu). Each object's live spatial POSITION is read best-effort from its panner/encoder FX (azimuth/elevation/distance) — who is loud AND where they are. Optionally pass bed:<track> to also meter the bed bus for a bed-vs-objects level reference. Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection); one synchronous render covers all objects, so keep the range short.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bed` | integer \| string | no | — |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `objects` | array&lt;['integer', 'string']&gt; | yes | — |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `bed`, `bedVsObjects`, `boundsFlag`, `count`, `detail`, `dryRun`, `error`, `interpretation`, `loudest`, `measuredSource`, `objects`, `plan`, `remediation`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bed": {
        "type": [
          "integer",
          "string"
        ]
      },
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "objects": {
        "items": {
          "type": [
            "integer",
            "string"
          ]
        },
        "maxItems": 128,
        "minItems": 1,
        "type": "array"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      }
    },
    "required": [
      "objects"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "bed": {
        "type": [
          "object",
          "null"
        ]
      },
      "bedVsObjects": {
        "type": [
          "number",
          "null"
        ]
      },
      "boundsFlag": {
        "type": "integer"
      },
      "count": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "interpretation": {
        "type": "string"
      },
      "loudest": {
        "type": "object"
      },
      "measuredSource": {
        "type": "string"
      },
      "objects": {
        "type": "array"
      },
      "plan": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.read_samples`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Direct, render-free PCM read of a track's item content or a take's source via a REAPER audio accessor (READ-ONLY). Reads a bounded window with NO render round-trip and reports per-channel sample peak / RMS / oversampled true-peak (dBTP) / K-weighted level, DC offset and full-scale clip count, plus an optional decimated min/max waveform overview. A TRACK target reads the summed media-item/take content BEFORE track FX/volume/pan; add itemIndex (and optional takeIndex) to read one take's source through its take FX. Window defaults to the whole source extent; bound it with start/duration (seconds) and set sampleRate to resample the read. Frame-accurate and fast — for post-FX bus/master metering use analysis.meter (which renders).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `clipThreshold` | number | no | default `0.999` |
| `dryRun` | boolean | no | default `false` |
| `duration` | number | no | — |
| `itemIndex` | integer | no | — |
| `overview` | boolean | no | default `false` |
| `overviewBuckets` | integer | no | default `512`; range [1, 8192] |
| `sampleRate` | integer | no | — |
| `start` | number | no | default `0` |
| `takeIndex` | integer | no | — |
| `target` | integer \| string | no | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `channels`, `channelsDetail`, `clamped`, `detail`, `dryRun`, `duration`, `error`, `frames`, `measuredSource`, `overview`, `plan`, `remediation`, `sampleRate`, `silent`, `source`, `sourceExtent`, `target`, `warnings`, `window`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "clipThreshold": {
        "default": 0.999,
        "type": "number"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "duration": {
        "type": "number"
      },
      "itemIndex": {
        "type": "integer"
      },
      "overview": {
        "default": false,
        "type": "boolean"
      },
      "overviewBuckets": {
        "default": 512,
        "maximum": 8192,
        "minimum": 1,
        "type": "integer"
      },
      "sampleRate": {
        "type": "integer"
      },
      "start": {
        "default": 0,
        "type": "number"
      },
      "takeIndex": {
        "type": "integer"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "channels": {
        "type": "integer"
      },
      "channelsDetail": {
        "type": "array"
      },
      "clamped": {
        "type": "boolean"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "duration": {
        "type": "number"
      },
      "error": {
        "type": "string"
      },
      "frames": {
        "type": "number"
      },
      "measuredSource": {
        "type": "string"
      },
      "overview": {
        "type": "object"
      },
      "plan": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "sampleRate": {
        "type": "integer"
      },
      "silent": {
        "type": "boolean"
      },
      "source": {
        "type": "string"
      },
      "sourceExtent": {
        "type": "object"
      },
      "target": {
        "type": "string"
      },
      "warnings": {
        "type": "array"
      },
      "window": {
        "type": "object"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.send_layout_inspect`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Read back the object + bed send layout feeding an external Dolby Atmos Renderer / DAPS input bus and QC it (the read-only counterpart to spatial.orchestrate_sends; the sibling of analysis.damf_inspect for routing). Decodes every incoming send on rendererTrack, reconstructs the 1-based renderer input roster, and flags problems: channel collisions (two sources on one channel), gaps (an unused channel below the top), objects over 118 / channels over 128, a bed of the wrong width or absent, non-mono objects, and — when expectedObjects is given — unrouted objects. Pass bedLayout (or bedChannels) to validate the bed; ok=true means the layout is a clean, spec-conformant master bus.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `bedChannels` | integer | no | min 0 |
| `bedLayout` | enum | no | one of: `5.1`, `7.1`, `7.1.2`, `7.1.4`, `9.1.6`, `22.2` |
| `expectedObjects` | integer | no | min 0 |
| `rendererTrack` | integer | yes | min 0 |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `bedDeclared`, `bedPresent`, `busChannels`, `coveredChannels`, `detail`, `error`, `issues`, `objectChannels`, `ok`, `receiveCount`, `receives`, `remediation`, `rendererTrack`, `summary`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "bedChannels": {
        "minimum": 0,
        "type": "integer"
      },
      "bedLayout": {
        "enum": [
          "5.1",
          "7.1",
          "7.1.2",
          "7.1.4",
          "9.1.6",
          "22.2"
        ],
        "type": "string"
      },
      "expectedObjects": {
        "minimum": 0,
        "type": "integer"
      },
      "rendererTrack": {
        "minimum": 0,
        "type": "integer"
      }
    },
    "required": [
      "rendererTrack"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "bedDeclared": {
        "type": [
          "integer",
          "null"
        ]
      },
      "bedPresent": {
        "type": "boolean"
      },
      "busChannels": {
        "type": "integer"
      },
      "coveredChannels": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "error": {
        "type": "string"
      },
      "issues": {
        "type": "array"
      },
      "objectChannels": {
        "type": "integer"
      },
      "ok": {
        "type": "boolean"
      },
      "receiveCount": {
        "type": "integer"
      },
      "receives": {
        "type": "array"
      },
      "remediation": {
        "type": "string"
      },
      "rendererTrack": {
        "type": "integer"
      },
      "summary": {
        "type": "string"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.spatial_field`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Ambisonic spatial-field analysis of an ACN/SN3D scene bus (READ-ONLY). Runs ONE bounded, BIT-EXACT analysis render (no normalization/limiting, so the inter-channel direction cues are preserved; snapshots + restores every RENDER_* field + selection; temp file deleted after reading), then computes from the samples: DirAC active-intensity DIRECTION-OF-ARRIVAL (azimuth/elevation in the ambisonic-standard convention — CCW from front, +az=left, matching spatial.ambisonic_encode / IEM / SPARTA), DIFFUSENESS ψ (0 = single plane wave, 1 = fully diffuse) and directivity 1−ψ; the Gerzon localization vectors — VELOCITY vector rV (\|rV\|=1 for a plane wave) and ENERGY vector rE via a 36-point virtual-speaker decode (sharper with order); and the per-ACN energy distribution. Order is inferred from the channel count ((order+1)² = 4/9/16/25…); pass normalization='n3d' if the bus is N3D. Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection). Fails closed if the target is not a valid ambisonic width (≥4 ch). TIMELINE: pass timeline:true for a WINDOWED trajectory — per-window DoA az/el, diffuseness, \|rV\|/\|rE\| — so a moving source traces its path; windowSec (default 1.0) and hopSec (default 0.5) shape the windows (hop auto-coarsens to cap the series at 400 windows).

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `hopSec` | number | no | default `0.5`; range [0.05, 30] |
| `normalization` | enum | no | one of: `sn3d`, `n3d`; default `"sn3d"` |
| `order` | integer | no | min 1 |
| `reSpeakers` | integer | no | default `36`; range [6, 256] |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |
| `target` | integer \| string | no | — |
| `timeline` | boolean | no | default `false` |
| `windowSec` | number | no | default `1.0`; range [0.1, 30] |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `acnEnergyFraction`, `acnUsed`, `boundsFlag`, `channels`, `detail`, `diffuseness`, `directivity`, `doa`, `dryRun`, `energyVector`, `error`, `interpretation`, `measuredSource`, `normalization`, `order`, `plan`, `remediation`, `target`, `timeline`, `velocityVector`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "hopSec": {
        "default": 0.5,
        "maximum": 30,
        "minimum": 0.05,
        "type": "number"
      },
      "normalization": {
        "default": "sn3d",
        "enum": [
          "sn3d",
          "n3d"
        ],
        "type": "string"
      },
      "order": {
        "minimum": 1,
        "type": "integer"
      },
      "reSpeakers": {
        "default": 36,
        "maximum": 256,
        "minimum": 6,
        "type": "integer"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "target": {
        "type": [
          "integer",
          "string"
        ]
      },
      "timeline": {
        "default": false,
        "type": "boolean"
      },
      "windowSec": {
        "default": 1.0,
        "maximum": 30,
        "minimum": 0.1,
        "type": "number"
      }
    },
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "acnEnergyFraction": {
        "type": "array"
      },
      "acnUsed": {
        "type": "integer"
      },
      "boundsFlag": {
        "type": "integer"
      },
      "channels": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "diffuseness": {
        "type": "number"
      },
      "directivity": {
        "type": "number"
      },
      "doa": {
        "type": "object"
      },
      "dryRun": {
        "type": "boolean"
      },
      "energyVector": {
        "type": "object"
      },
      "error": {
        "type": "string"
      },
      "interpretation": {
        "type": "string"
      },
      "measuredSource": {
        "type": "string"
      },
      "normalization": {
        "type": "string"
      },
      "order": {
        "type": "integer"
      },
      "plan": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "target": {
        "type": "string"
      },
      "timeline": {
        "type": "object"
      },
      "velocityVector": {
        "type": "object"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

#### `analysis.stem_loudness`

**Profile:** `analysis` · **Hints:** read-only, idempotent

Per-stem loudness for a LIST of tracks in ONE bounded render pass (READ-ONLY) — the object/stem balance readout for immersive sessions. Renders the given tracks as BIT-EXACT multichannel stems (selected-track stems, no normalize/limit; snapshots + restores every RENDER_* field + selection; temp files deleted after reading), then computes per stem in C++: ITU-R BS.1770-4 GATED integrated loudness (-70 LKFS absolute + -10 LU relative gate, channel weights from the SMPTE bed labels, LFE excluded), ungated level, activity fraction (how much of the range the stem is audibly active), oversampled true peak (dBTP) and sample peak, plus each stem's offset from the LOUDEST stem (deltaFromLoudestLu). Program loudness of the MIX belongs to analysis.meter; this tool ranks the parts. Bound the range with boundsFlag (0 = custom startPos/endPos, 1 = ENTIRE PROJECT — the default, 2 = time selection) — one synchronous render covers all stems, so keep the range short.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `boundsFlag` | integer | no | default `1`; range [0, 7] |
| `dryRun` | boolean | no | default `false` |
| `endPos` | number | no | — |
| `renderAction` | integer | no | default `41824` |
| `startPos` | number | no | — |
| `tracks` | array&lt;['integer', 'string']&gt; | yes | — |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `boundsFlag`, `count`, `detail`, `dryRun`, `error`, `loudest`, `measuredSource`, `plan`, `remediation`, `stems`, `warnings`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "boundsFlag": {
        "default": 1,
        "maximum": 7,
        "minimum": 0,
        "type": "integer"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "endPos": {
        "type": "number"
      },
      "renderAction": {
        "default": 41824,
        "type": "integer"
      },
      "startPos": {
        "type": "number"
      },
      "tracks": {
        "items": {
          "type": [
            "integer",
            "string"
          ]
        },
        "maxItems": 32,
        "minItems": 1,
        "type": "array"
      }
    },
    "required": [
      "tracks"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "boundsFlag": {
        "type": "integer"
      },
      "count": {
        "type": "integer"
      },
      "detail": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "loudest": {
        "type": "object"
      },
      "measuredSource": {
        "type": "string"
      },
      "plan": {
        "type": "string"
      },
      "remediation": {
        "type": "string"
      },
      "stems": {
        "type": "array"
      },
      "warnings": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

### Composite / DSL

_The deterministic composite macro-DSL runner (`$ref`/capture, atomic single-undo)._

#### `session.run_dsl`

**Profile:** `full` · **Hints:** mutating

Run a deterministic macro-DSL script: one `verb key=val` line per tool call, composing the composite outcome verbs (spatialize, to_ambisonic, immersive_session, apply_style, check_deliverable) plus curated primitives (build_bed, detect_suites, add_monitor, rotate, render, track_add, set_channels, select). A verb may also be a fully-qualified tool name (e.g. spatial.build_bed). Values coerce deterministically: true/false/null, numbers, "quoted strings", comma,lists -> arrays, and [..]/{..} literal JSON; dotted keys nest (target.layout=7.1.4), and sugar keys map to the schema (layout/order/spec/style/…). No LLM is in the path. dryRun previews every step (composites dry-run, read-only tools run, mutating primitives are listed) and mutates nothing. On apply the whole script is ONE undo point; atomic (default) rolls the entire run back if a step fails, else it keeps what applied and reports how far it got. A line may capture its result — `$name = verb …` — and a later value may reference it as $name or $name.dotted.path (resolved at apply from the captured result). Comments start with #.

**Parameters**

| Param | Type | Required | Notes |
| --- | --- | --- | --- |
| `atomic` | boolean | no | default `true` |
| `dryRun` | boolean | no | default `false` |
| `script` | string | yes | — |
| `stopOnError` | boolean | no | default `true` |

_Additional properties: not allowed._

**Returns**

Returns a structured object with: `atomic`, `detail`, `diff`, `dryRun`, `error`, `errors`, `executed`, `failedStep`, `line`, `note`, `ok`, `plan`, `remediation`, `rolledBack`, `stepCount`, `stepError`, `steps`, `undo`, `unknown`.

<details><summary>Full JSON schema</summary>

```json
{
  "inputSchema": {
    "additionalProperties": false,
    "properties": {
      "atomic": {
        "default": true,
        "type": "boolean"
      },
      "dryRun": {
        "default": false,
        "type": "boolean"
      },
      "script": {
        "type": "string"
      },
      "stopOnError": {
        "default": true,
        "type": "boolean"
      }
    },
    "required": [
      "script"
    ],
    "type": "object"
  },
  "outputSchema": {
    "properties": {
      "atomic": {
        "type": "boolean"
      },
      "detail": {
        "type": "string"
      },
      "diff": {
        "type": "string"
      },
      "dryRun": {
        "type": "boolean"
      },
      "error": {
        "type": "string"
      },
      "errors": {
        "type": "array"
      },
      "executed": {
        "type": "array"
      },
      "failedStep": {
        "type": "integer"
      },
      "line": {
        "type": "integer"
      },
      "note": {
        "type": "string"
      },
      "ok": {
        "type": "boolean"
      },
      "plan": {
        "type": "array"
      },
      "remediation": {
        "type": "string"
      },
      "rolledBack": {
        "type": "boolean"
      },
      "stepCount": {
        "type": "integer"
      },
      "stepError": {
        "type": "object"
      },
      "steps": {
        "type": "array"
      },
      "undo": {
        "type": "string"
      },
      "unknown": {
        "type": "array"
      }
    },
    "type": "object"
  }
}
```
</details>

## Resources

Read-only, addressable snapshots of REAPER state (`resources/list`, `resources/read`, `resources/templates/list`). Templates carry `{placeholders}` resolved at read time. The project-state and routing-graph resources support subscriptions (`resources/subscribe`); the SSE channel flushes `notifications/resources/updated` when they change.

| URI | Name | Type | MIME | Description |
| --- | --- | --- | --- | --- |
| `reaper://project/state` | Project state snapshot | static | `application/json` | Project name, length, tempo, play state, and a per-track summary (name, channels, volume dB, pan, mute, selection). |
| `reaper://routing/graph` | Routing graph | static | `application/json` | The project's routing graph: nodes (tracks + master, with channel counts and folder/main-send flags) and edges (track-to-track sends and hardware outputs). The substrate the immersive layer builds beds and ambisonic pin maps on. |
| `reaper://track/{index}/chunk` | Track .RPP state chunk | template | `text/plain` | The raw REAPER project (.RPP) state chunk for the track at {index} (0-based) — the exact text REAPER serializes to the project file, including FX, sends, and envelopes. |

## Prompts

Declarative expert workflows (`prompts/list`, `prompts/get`). Selecting one injects a message that steers the agent to call the semantic verbs in the right order — an expert immersive workflow without knowing the verb names.

### `encode_to_ambisonics` — Encode a source to an ambisonic scene

Encode a mono/stereo bus (or spatialize multiple stems) into an order-N ambisonic scene (ACN/SN3D) via spatial.stereo_to_ambisonic / spatial.spatialize_stems.

| Argument | Required | Description |
| --- | --- | --- |
| `sourceBus` | yes | Track/bus to encode (name or index) |
| `order` | no | Ambisonic order, e.g. 1 or 3 (default 1) |
| `normalization` | no | SN3D or FuMa (default SN3D) |
| `monitor` | no | Binaural monitor: 'binaural' or 'none' (default binaural) |

### `master_for_delivery` — Master to a deliverable spec

Apply an immersive-aware master chain and check the result against a named loudness/true-peak deliverable spec (atmos-music, atsc-a85, ebu-r128, streaming-stereo, …) via mix.apply_style + analysis.check_deliverable.

| Argument | Required | Description |
| --- | --- | --- |
| `spec` | yes | Deliverable spec name, e.g. atmos-music, atsc-a85, streaming-stereo |
| `target` | no | Track/bus to master (default: the master) |

### `setup_atmos_session` — Set up an Atmos bed + objects session

Scaffold an immersive Dolby Atmos session — a bed, N object tracks, a binaural monitor, and the external Dolby Renderer/DAPS send layout — via spatial.setup_immersive_session.

| Argument | Required | Description |
| --- | --- | --- |
| `beds` | no | Bed layout, e.g. 7.1.4 or 9.1.6 (default 7.1.4) |
| `objects` | no | Number of object tracks to create (default 0) |
| `monitor` | no | Binaural monitor: 'binaural' or 'none' (default binaural) |

---

_Reference generated 2026-07-10 from the live registry (`cmake --build build --target reference-doc`). See `docs/CONVENTIONS.md` for channel-order, coordinate, bed-layout, and loudness-spec conventions, and `SECURITY.md` for the transport threat model._
