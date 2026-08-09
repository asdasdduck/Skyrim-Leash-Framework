Scriptname LeashFramework Hidden

;/
Active leash factions supplied by Leash.esm:
- LeashedFaction (form ID 0x000D6A): contains actors that currently have an active leash.
- LeasherFaction (form ID 0x000D6B): contains actors that currently hold one or more active leashes.

Leash lifecycle mod events:
- LeashFramework_OnLeash: sent when a leash is successfully applied, replaced, or restored from a save.
- LeashFramework_OnUnleash: sent when a leash is disconnected, removed by UnleashAll, or replaced.

Both lifecycle events use the leashed Actor as sender and set numArg to 0.0. strArg describes why the
event was sent. OnLeash uses "applied", "replaced", or "loaded". OnUnleash uses "disconnected",
"unleashAll", or "replaced". Replacing a leash sends OnUnleash before OnLeash. Internal state cleanup
while starting or loading a game does not send OnUnleash.

Pull mod events:
- LeashFramework_OnActorPulled: sent once when normal direct-locomotion pulling starts.
- LeashFramework_OnActorRagdollPulled: sent once when forced ragdoll pulling starts.

Register for these events with RegisterForModEvent. Both pull events use the leashed Actor as sender, leave
strArg empty, and provide the holder/anchor-to-collar distance at the transition in numArg. A pull event can
be sent again only after that pull mode stops and later starts again; it is not sent every frame while pulling
remains active.
/;

;/
Connects a leash from a holder's right hand to leash bones on another actor.

holder: Actor holding the leash. The current attachment bone is NPC R Hand [RHnd].
leashed: Actor wearing the leash mesh. An actor can have only one active leash.
parentBone: Exact node name to find on the leashed actor before searching its descendants.
leashBoneMatch: Text found anywhere in each ordered leash bone name beneath parentBone. For example,
Main_ matches hdtSSEPhysics_AutoRename_Armor_00000004 Main_01 after SMP renames the node.
minLength: Distance where active leash pulling stops. Must be zero or greater.
maxLength: Maximum holder-to-collar distance. Must be positive and at least minLength.
persistent: When true, the leash is saved and restored until explicitly disconnected.

Applying another leash to the same leashed actor replaces its current leash.
Returns true when the arguments are valid and the leash is accepted.
/;
Bool Function ApplyLeash(Actor holder, Actor leashed, String parentBone, String leashBoneMatch, Float minLength, Float maxLength, Bool persistent) Global Native

;/
Connects a leash from either hand on a holder to leash bones on another actor and applies the closed-fist grip.

rightHand: When true, uses NPC R Hand [RHnd]. When false, uses NPC L Hand [LHnd].
All other arguments and replacement behavior match ApplyLeash.

Returns true when the arguments are valid and the leash is accepted.
/;
Bool Function ApplyLeashToHand(Actor holder, Actor leashed, String parentBone, String leashBoneMatch, Float minLength, Float maxLength, Bool persistent, Bool rightHand = True) Global Native

;/
Connects a leash from an exact bone on a holder to leash bones on another actor.

holderBone: Exact node name on the holder. This attachment does not alter the holder's hand pose.
offsetX, offsetY, offsetZ: Optional local-space offset from holderBone. The offset follows the bone's
translation, rotation, and scale, but does not add an attachment rotation.
All other arguments and replacement behavior match ApplyLeash.

Returns true when the arguments are valid and the leash is accepted.
/;
Bool Function ApplyLeashToBone(Actor holder, Actor leashed, String holderBone, String parentBone, String leashBoneMatch, Float minLength, Float maxLength, Bool persistent, Float offsetX = 0.0, Float offsetY = 0.0, Float offsetZ = 0.0) Global Native

;/
Connects a leash mesh equipped by the holder to an exact bone on the leashed actor.

The first ordered leash bone remains in its neutral animated pose on the holder. The last ordered leash
bone attaches to leashedAttachmentBone.

leashParentBone: Exact node name to find on the holder before searching its descendants for leash bones.
leashedAttachmentBone: Exact node name on the leashed actor.
attachmentOffsetX, attachmentOffsetY, attachmentOffsetZ: Optional local-space offset from
leashedAttachmentBone. The offset follows the bone's translation, rotation, and scale, but does not add
an attachment rotation.
closedHand: Applies the closed-fist pose to the holder. 1 closes the right hand, 2 closes the left hand,
and every other value leaves both hands unchanged.
All other arguments and replacement behavior match ApplyLeash.

Returns true when the arguments are valid and the leash is accepted.
/;
Bool Function ApplyHolderOwnedLeashToBone(Actor holder, Actor leashed, String leashedAttachmentBone, String leashParentBone, String leashBoneMatch, Float minLength, Float maxLength, Bool persistent, Float attachmentOffsetX = 0.0, Float attachmentOffsetY = 0.0, Float attachmentOffsetZ = 0.0, Int closedHand = 0) Global Native

;/
Connects a holderless leash from a fixed position to leash bones on an actor.

anchorCell: Cell containing the world-space position. It must correspond to the supplied coordinates.
x, y, z: Fixed world-space coordinates of the leash anchor.
All other arguments and replacement behavior match ApplyLeash.
Pull distances are measured from the fixed anchor instead of a holder.

World-position leashes do not assign LeasherFaction, have no result from GetLeashHolder, and do not
use holder teleport recovery. Use DisconnectLeash(None, leashed) to disconnect one.
Returns true when the arguments are valid and the leash is accepted.
/;
Bool Function ApplyLeashAtPosition(Actor leashed, Cell anchorCell, Float x, Float y, Float z, String parentBone, String leashBoneMatch, Float minLength, Float maxLength, Bool persistent) Global Native

;/
Disconnects the leash matching the specific holder and leashed actor.

holder: Actor holding the leash. Pass None for a holderless world-position leash.
leashed: Actor connected as the leashed endpoint.

Returns true when the matching leash was disconnected.
/;
Bool Function DisconnectLeash(Actor holder, Actor leashed) Global Native

;/
Disconnects every leash where actor is either the holder or the leashed actor.

Returns true when at least one leash was disconnected.
/;
Bool Function UnleashAll(Actor actor) Global Native

;/
Returns true when actor currently has an active leash.
/;
Bool Function IsLeashed(Actor actor) Global Native

;/
Returns true when actor currently holds at least one active leash.
/;
Bool Function IsLeashHolder(Actor actor) Global Native

;/
Returns the actor holding leashed's active leash, or None when the leash is holderless or absent.
/;
Actor Function GetLeashHolder(Actor leashed) Global Native

;/
Returns every actor currently leashed to holder. Returns an empty array when holder has no active leashes.
/;
Actor[] Function GetLeashedActors(Actor holder) Global Native

;/
Returns the minimum length of leashed's active leash, or -1.0 when leashed has no active leash.
/;
Float Function GetMinLeashLength(Actor leashed) Global Native

;/
Returns the maximum length of leashed's active leash, or -1.0 when leashed has no active leash.
/;
Float Function GetMaxLeashLength(Actor leashed) Global Native

;/
Sets the minimum length of leashed's active leash.

newLength must be zero or greater and cannot exceed the current maximum length.
Returns false when leashed has no active leash or newLength is invalid.
/;
Bool Function SetMinLeashLength(Actor leashed, Float newLength) Global Native

;/
Sets the maximum length of leashed's active leash.

newLength must be positive and cannot be less than the current minimum length.
Returns false when leashed has no active leash or newLength is invalid.
/;
Bool Function SetMaxLeashLength(Actor leashed, Float newLength) Global Native
