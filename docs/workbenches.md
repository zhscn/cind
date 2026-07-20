# Workbenches

A workbench is a durable editing surface over the editor's global Buffer and Project registries.
It combines an ordered project scope, one WindowLayout, per-workbench Buffer recency, an active
Window, and named Window slots. A Project remains a tooling and configuration boundary; it does
not own the workbench or its layout.

Guile owns workbench names, name uniqueness, project scope, Buffer recency, Window roles, pinning,
display-policy provenance, role-derived Window slots, the active workbench, each workbench's active
Window, and each Window's ordered jump walk and cursor. Native workbench objects own the
WindowLayout and jump graph nodes, edges, Buffer anchors and fallback positions.

The editor always owns at least one workbench and has exactly one active workbench. Switching
workbenches changes the presented layout without destroying inactive Windows or Views. Their
carets, selections, input states and viewports therefore remain available when the workbench is
selected again. Buffers remain global and may be displayed by Views in several workbenches; edits
to a shared Buffer are immediately visible through each View.

The Guile registry preserves workbench creation order. Closing the active workbench selects the
next entry in that order and wraps at the end before releasing the closing state.

Guile selection state references native generational IDs. The application validates those IDs
against the workbench registry and WindowLayout whenever it crosses the policy boundary. Window
deletion transfers focus before removing a layout leaf, and workbench closure selects a replacement
before releasing the active workbench state.

## Membership and project scope

A workbench's visible Buffer set is the union of:

- every open Buffer attached to a Project in the declared scope;
- the workbench MRU, which records each Buffer displayed in that workbench.

Displaying a Buffer always visits it in the target workbench. A file outside the declared scope is
therefore a visitor: it is available to Buffer switching in that workbench without changing its
Project or adopting that Project. `C-x b` uses this scoped set, and its picker can widen to the
global Buffer pool with another `C-x b`.

The scope may contain several Projects. Project file selection and search use all scoped Projects,
which supports one editing surface spanning related repositories. Adopting a Project does not
remove it from another workbench; Scheme policy may impose a narrower convention when desired.

## Lifecycle commands

The `C-x w` prefix owns the default workbench commands:

| Key | Command | Effect |
| --- | --- | --- |
| `n` | `workbench.new` | Create and select a named workbench |
| `s` | `workbench.switch` | Select a workbench through the picker |
| `k` | `workbench.close` | Close the active workbench and release its Windows and Views |
| `a` | `workbench.adopt-project` | Add a Project to the active scope |
| `e` | `workbench.expel` | Remove the current Buffer from the active MRU |
| `r` | `window.set-role` | Assign a named placement role to the active Window |
| `p` | `window.toggle-pinned` | Toggle policy replacement protection |
| `d` | `window.dismiss` | Delete a policy-created Window or clear an ordinary role |
| `S` | `workbench.save-session` | Serialize all workbenches to a selected file |
| `R` | `workbench.restore-session` | Restore all workbenches from a selected file |

The default presentation includes the workbench name in the modeline only when more than one
workbench exists.

## Buffer placement

Display requests carry an intent and their origin Window. The bundled Scheme policy maps intents
to a deterministic reuse or split plan:

| Intent | Default placement |
| --- | --- |
| `edit` | Reuse the active Window |
| `jump` | Reuse an existing `jump` slot; otherwise reuse the active Window unless pinned or a tool Window, then use an adjacent Window or create the slot |
| `tools` | Reuse or create a bottom `tools` slot |
| `doc` | Reuse or create a bottom `doc` slot |
| `pop` | Split beside the active Window |
| `explicit` | Use the Window named by the caller |

A role is the Window-side representation of a workbench slot. Each role is unique within its
workbench, so assigning it to another Window moves the derived slot. A pinned Window is not
selected by policy for replacement. Windows created by placement policy retain provenance so
dismissing them removes their layout leaf; dismissing an ordinary role-bearing Window clears the
role and preserves the split.

The display policy is a replaceable Guile procedure. Native code validates its complete plan,
applies WindowLayout invariants, and provides a deterministic fallback when the procedure reports
an error. Guile jump policy decides whether the display intent contributes to the target Window's
jump walk and assigns a semantic edge kind to each recorded transition; the native jump graph
retains the resulting nodes and edge.

The origin Window also identifies the target workbench. An asynchronous open or tool request may
finish while another workbench is active; its result updates the originating layout and MRU without
changing the application's active workbench. A destroyed origin falls back to the active workbench.

## Persistent sessions

The session format stores stable values rather than runtime IDs:

- workbench names and the active workbench index;
- scoped Project root paths and MRU resource paths;
- the complete split tree, branch axes and ratios;
- each leaf's resource path, caret byte offset, role, pinned state and policy provenance;
- durable jump graph nodes and edges, plus each leaf's bounded jump walk;
- the active leaf in each workbench.

Session file reads and writes use the asynchronous runtime. The native preparation mechanism
validates the entire serialized structure, constructs replacement layouts beside the current
state, then atomically selects the restored registry. It returns a resource-loading plan containing
stable paths, target Windows and MRU membership. The Guile policy owns the plan's task set,
replacement cancellation, unavailable-resource count, Buffer creation, exact Window placement,
MRU finalization and user feedback. Resource-backed Buffers are deduplicated by stable resource
identity. Missing files leave a usable fallback Buffer in their Window and do not prevent other
workbench state from being restored. MRU-only visitors are loaded even when no restored Window
displays them.

`workbench-session-state` exposes serialization to Guile, and the `(cind core)`
`restore-workbench-session!` policy composes native topology preparation with asynchronous resource
loading. The bundled save and restore commands own path selection and session-file I/O.

## Inspector state

`editor.workbenches` exposes every active and inactive workbench, including scope and MRU IDs,
active Window, named slots, the complete layout tree, and each retained Window/View/Buffer binding.
The inspector validates unique ownership of Windows, active identities, layout leaves, MRU/scope
uniqueness and the correspondence between roles and slots.
