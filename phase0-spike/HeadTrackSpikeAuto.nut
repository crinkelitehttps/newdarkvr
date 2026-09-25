// Phase 0 feasibility spike (see docs/DEVLOG.md
// and phase0-spike/SETUP.md, HANDOFF.md).
//
// Runs 1-2 (single +30 deg toggle) showed that player MOVEMENT stays on the
// body heading while the camera is DynamicAttach()'d to an offset marker, but
// that the frob highlight is lost -- suggesting frob targeting follows the
// CAMERA. Confound: the marker was placed at the player's position, not the
// camera's eye pose. This is the CONTROL run that removes that confound.
//
// Attached (via miss_all.dml) to a throwaway Marker "HeadTrackController";
// the camera is attached to a second Marker "HeadMarker".
//
// Cycles through three 10-second phases, forever, announced on screen:
//   0 NORMAL   camera on the player. Stand still and aim at a frobbable item
//              until it is highlighted. At the END of this phase the exact
//              camera pose (Camera.GetPosition/GetFacing) is captured.
//   1 CONTROL  camera on the marker at that EXACT pose, yaw offset 0. The
//              view should look identical to NORMAL. If the highlight is lost
//              here, merely attaching the camera elsewhere breaks frob.
//   2 TEST     same pose, yaw +30 deg. Highlight lost here but kept in
//              CONTROL => frob follows the camera's direction.
// Note the item highlight in each phase, and walk with W in any phase.
//
// Squirrel's SetOneShotTimer period is in SECONDS (the first version passed
// 5000 and never fired). Camera.LockMovement is deliberately not used.
// Everything is also logged to Thief2.log via Debug.Log, including who owns
// the camera at the end of each phase (to show engine-side camera resets).

class HeadTrackSpikeAuto extends SqRootScript
{
	function Say(text)
	{
		Debug.Log("HeadTrackSpikeAuto: " + text);
		DarkUI.TextMessage(text, 0, 9000);
	}

	function V(v)
	{
		return "(" + v.x + ", " + v.y + ", " + v.z + ")";
	}

	function OnSim()
	{
		if (message().starting)
		{
			SetData("phase", 0);
			SetOneShotTimer("HeadTrackPhase", 10);
			Say("phase 0 NORMAL: stand still, aim at an item until it highlights");
		}
	}

	function OnTimer()
	{
		if (message().name != "HeadTrackPhase")
			return;

		// Re-arm first so an error below cannot silently stop the cycle.
		SetOneShotTimer("HeadTrackPhase", 10);
		try
		{
			Advance();
		}
		catch (e)
		{
			Debug.Log("HeadTrackSpikeAuto: ERROR in Advance(): " + e);
		}
	}

	function Advance()
	{
		local player = ObjID("Player");
		local marker = Object.Named("HeadMarker");
		if (marker == 0)
		{
			Debug.Log("HeadTrackSpikeAuto: 'HeadMarker' not found -- did miss_all.dml load?");
			return;
		}

		local phase = GetData("phase");
		local parent = Camera.GetCameraParent();
		Debug.Log("HeadTrackSpikeAuto: end of phase " + phase + ": camera parent=" + parent
			+ " (player=" + player + ", marker=" + marker + ")"
			+ " cam pos=" + V(Camera.GetPosition()) + " cam facing=" + V(Camera.GetFacing())
			+ " | player facing=" + V(Object.Facing(player)));

		local next = (phase + 1) % 3;

		if (phase == 0)
		{
			// Camera is on the player: capture its exact pose for the marker.
			local p = Camera.GetPosition();
			local f = Camera.GetFacing();
			SetData("px", p.x); SetData("py", p.y); SetData("pz", p.z);
			SetData("fx", f.x); SetData("fy", f.y); SetData("fz", f.z);
			Debug.Log("HeadTrackSpikeAuto: captured eye pose pos=" + V(p) + " facing=" + V(f));
		}

		if (next == 0)
		{
			Camera.DynamicAttach(player);
			Say("phase 0 NORMAL: stand still, aim at an item until it highlights");
		}
		else
		{
			local yaw = (next == 2) ? 30 : 0;
			local pos = vector(GetData("px"), GetData("py"), GetData("pz"));
			local facing = vector(GetData("fx"), GetData("fy"), GetData("fz") + yaw);
			Object.Teleport(marker, pos, facing, 0);
			Camera.DynamicAttach(marker);
			if (next == 1)
				Say("phase 1 CONTROL: camera on marker, yaw +0. Is the item still highlighted?");
			else
				Say("phase 2 TEST: yaw +" + yaw + ". Is the item still highlighted?");
		}

		SetData("phase", next);
	}
}
