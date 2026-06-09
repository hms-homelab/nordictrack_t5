/*
 * Treadmill ESP32-S3 SuperMini Enclosure — SINGLE BOARD variant
 * Version: 1.0.0  (2026-06-06)
 *   - Adapted from the cpapdash Push-C3 BARE BOARDS enclosure
 *     (push-c3-enclosure-bareboards.scad v1.0.9).
 *   - Reduced from TWO boards to ONE:
 *       * single floor-post set (4 posts)
 *       * single lid-post set (4 posts)
 *       * single USB-C cutout (one short wall only)
 *   - Resized for an ESP32-S3 SuperMini, which is slightly LONGER on its
 *     long axis than the C3 SuperMini. The board's long axis (with the
 *     USB-C end) is parameterized as `board_w_x` — bump it for the S3.
 *   - Kept from the C3 design (unchanged behavior):
 *       * v1.0.8 plug-body USB-C cutout 12.5 x 6.8 mm
 *       * back constraint ribs (single board now)
 *       * printed-pin hinge (1.75 mm filament axle)
 *       * front snap-fit clip (no hardware)
 *       * lid heat-relief grill (oval pattern of 1.9 mm holes)
 *
 * ---- ESP32-S3 SuperMini board ----
 * The S3 SuperMini is ~22.5-25 mm long depending on the variant; it is
 * slightly LONGER on the long axis than the C3 SuperMini (22.5 mm). The
 * USB-C connector is on a SHORT edge (so the USB-C extends along the
 * board's LONG axis = box X direction = out through the X-wall cutout),
 * with a ~2 mm overhang past the PCB edge. Default board long axis here
 * is 25 mm; measure your board and adjust `board_w_x` if needed.
 *
 * Single-board features:
 *   - 4 floor posts: tiny pillars on the box floor that the PCB rests on,
 *     lifted 2 mm off the floor for solder-joint clearance.
 *   - 4 lid posts: matching pillars on the lid that press DOWN on the PCB
 *     corners when the lid closes, so the board can't lift or shift.
 *
 * Closure:
 *   - Back lid-body hinge: printed-knuckle, 1.75 mm filament axle pin.
 *   - Front: tiny printed snap-fit clip (no hardware) — body has a small
 *     ridge on the outside front wall, lid has a downward tab with a
 *     notch that catches the ridge when shut.
 *   - Library "draw" / "clip" latches are disabled via module overrides.
 *
 * Other features:
 *   - USB-C cutout in ONE short wall (12.5 x 6.8 mm, fits the plug body).
 *   - Lid heat-relief grill: oval pattern of 1.9 mm holes.
 *
 * Hardware list (no metal hardware):
 *   - Back hinge axle: 1 x ~14 mm length of 1.75 mm 3D-printer filament
 *     (any color works; cut to length to match the hinge knuckle width).
 *
 * Based on smkent/monoscad rugged-box (CC 4.0 BY-SA)
 * https://github.com/smkent/monoscad/tree/main/rugged-box
 */

include <rugged-box-library.scad>;

// Override the hinge axle from M3 screw → 1.75 mm filament pin.
// Setting screw_diameter=1.85 (filament 1.75 + 0.1 clearance) shrinks the
// hole AND would shrink the eyelet body; bump screw_eyelet_size_proportion
// to keep the eyelet/knuckle thick enough to be strong.
// screw_hole_diameter_size_tolerance is positive so the hole is loose
// enough for the filament pin to spin freely (vs the default -0.1 which
// is intentionally undersized for thread-forming on a real M3 screw).
screw_diameter = 1.85;
screw_eyelet_size_proportion = 5.0;
screw_hole_diameter_size_tolerance = 0.10;

// No side ribs — the USB-C cutout interrupts a wall position
function rb_side_rib_positions() = [];

// Disable the rugged-box library's latch entirely — overridden by the
// printed snap-fit clip below. No M3 latch screws needed (only the back
// hinge axle filament pin remains).
module _box_latch_ribs() {}
module _latch(placement="default") {}
function _compute_latch_count(latch_count = 0) = 0;

// Silence the library's BOM echo (which would say "0 M3x..." — misleading
// because the formula counts the back hinge screw inside the latch
// formula). Print our own correct hardware list instead.
module rbox_bom() {}
echo("---- HARDWARE NEEDED ----");
echo(str("Back hinge axle: 1 x length of 1.75mm 3D-printer filament (cut to ~", Latch_Width, "mm to match the hinge knuckle width)"));
echo(str("Hinge hole diameter: ", screw_diameter + screw_hole_diameter_size_tolerance, "mm"));
echo("Snap-fit front catch — no hardware");

/* [Rendering] */
Part = "assembled_open"; // [bottom: Bottom, top: Top, latch: Latch, side-by-side: Side by Side, assembled_open: Assembled Open, assembled_closed: Assembled Closed]

/* [Board Dimensions — ESP32-S3 SuperMini] */
// Board dim in X — the LONG axis (with the USB-C end). The S3 SuperMini
// is slightly LONGER than the C3 (22.5 mm). Default 25 mm; measure yours
// and adjust. The USB-C connector sits on a SHORT edge, so this long axis
// points out through the X-wall USB-C cutout.
board_w_x        = 23;           // S3 long axis (measured 22.96) — was 22.5 on C3
// Board dim in Y — the SHORT axis (board width). Measured 17.98.
board_l_y        = 18;
pcb_thickness    = 1.6;
// USB-C connector overhang past the PCB short edge (measured front-of-port
// to PCB edge). Used to place the board so the receptacle sits flush.
usb_overhang     = 1.85;

/* [Box Dimensions — sized for ONE bare SuperMini board, no carrier] */
// Interior width (X) — board's LONG axis + 0.75 mm clearance per side at
// lip Z. Snug fit on the BACK of the board (opposite the USB-C short
// edge). Auto-derived from board_w_x so resizing the board resizes the box.
Width = board_w_x + 1.5;          // 26.5 for a 25 mm board (was Width=24 on C3)
// Interior length (Y) — one board × 18 mm + ~3 mm margin per board edge at
// lip Z. Generous enough that sharp PCB corners clear the lip-Z corners.
Length = board_l_y + 6;           // 24 for an 18 mm board
// Interior bottom height (PCB stack + USB-C body + small clearance)
//   needed: 2 (floor post) + 1.6 (PCB) + 3.26 (USB-C body) ≈ 7 mm,
//   plus room for the USB-C cutout (6.8 mm tall, centered at usbc_z) to
//   fit fully within the body wall → body needs ≥ ~9 mm.
Bottom_Height = 9;
// Interior top height (lid clearance — minimal since no components on top)
Top_Height = 2;

/* [Hardware] */
Corner_Radius = 3;
// Walls 2.0 mm (rigid, low material)
Wall_Thickness = 2.0;
Lip_Thickness = 2.0;
// Latch / hinge width — fits within the back wall.
//   (Length=24, wall+lip=4 → 16 mm clear, use 12 mm hinge w/ 2 mm margin each side)
Latch_Width = 12;
// Distance hinge↔catch on the latch — keep small so the latch fits the
// shorter total height (Bottom + Top = 11 mm internal, plus walls).
Latch_Screw_Separation = 10;

/* [USB-C Cutout] */
// SIZED FOR THE FEMALE RECEPTACLE APERTURE (the female port IS the opening).
// This now works because the board is shifted toward the -X wall (board_box_x
// below) so the receptacle front sits ~0.3 mm shy of the outer face — flush.
// The C3 design used a plug-body cutout because its centered board left the
// receptacle ~2.75 mm recessed; here the overhang (1.85) ≈ wall (2.0), so the
// port reaches the wall and a plug mates directly into it.
//   Measured female opening: 8.9 x 3.2 mm → + tolerance:
usbc_opening_w = 9.2;   // 8.9 + 0.3 mm
usbc_opening_h = 3.6;   // 3.2 + 0.4 mm
// Z center: receptacle sits on the PCB top. Bare-board stack:
// 2(wall) + 2(floor post) + 1.6(PCB) = PCB top at 5.6; receptacle ~3.2 tall
// → center at 5.6 + 1.6 = 7.2.
usbc_z = 7.2;

/* [Lid Heat-Relief Grill] */
// Square-hole waffle grill is disabled (the small lid doesn't have room
// around the lid posts). Heat relief is provided by the oval hole pattern
// (heat_relief_holes) below. Set grill_cols/grill_rows > 0 to re-enable
// the square waffle grill if you enlarge the box.
grill_sq = 3;
grill_gap = 2;
grill_cols = 0;   // 0 disables the square waffle grill (loop range empty)
grill_rows = 0;

/* [Bottom Label] */
Bottom_Label_Text = "Treadmill S3";
// Text height (mm) — sized to fit the small bottom face. 4 mm overran the
// bottom; 2.2 mm fits "Treadmill S3" within the ~23 x 18 bottom face.
Bottom_Label_Size = 2.2;
// Recess depth (mm)
Bottom_Label_Depth = 0.6;

/* [Snap-fit Catch] */
// Replaces the rugged-box library latch. Body has a small ridge on the
// outside front wall; lid has a TAPERED downward tab with a notch that
// catches the body ridge. Thicker at the lid root (stiff in the notch
// region — resists prying open), thinner at the tip (flexes easily to
// snap shut). The notch is in the upper (thicker) half of the tab.
snap_w = 10;                  // X width across the front wall (was 12 on C3)
snap_protrusion = 0.7;        // body ridge depth
snap_ridge_h = 1.0;           // ridge height in Z
snap_tab_thick_root = 2.5;    // lid tab thickness in Y at the root (lid)
snap_tab_thick_tip = 1.5;     // lid tab thickness in Y at the tip
snap_tab_drop = 7;            // tab length below the lid mating face
snap_notch_h = 2.0;           // notch height in Z (larger than ridge for tolerance)

// --- Computed ---
board_box_y = 0;   // single board centered on the short axis (Y)
// Shift the board toward the USB-C (-X) wall so the receptacle ends ~flush
// with the outer wall. Per-side interior gap = (Width - board_w_x)/2 = 0.75;
// leave 0.15 mm residual gap on the USB side → board_box_x = -0.60. Net
// receptacle recess from the outer face = (Wall_Thickness + 0.15) - usb_overhang
// = (2.0 + 0.15) - 1.85 = 0.30 mm (flush; a plug seats fine). The back of the
// board (now +1.2 mm gap) is held by the back rib.
board_box_x = -((Width - board_w_x) / 2 - 0.15);
// Outer side wall X.
wall_x = Width / 2 + Wall_Thickness + Lip_Thickness;
cut_depth = Wall_Thickness + Lip_Thickness + 6;
// Outside front wall Y position. The library's $b_outer_length includes
// (wall_thickness + lip_thickness) on each side — NOT just wall_thickness.
front_wall_y = -(Length / 2 + Wall_Thickness + Lip_Thickness);
// Body bump Z position — 2 mm below top of body so the ridge is hidden
// inside the lid's tab when fully closed.
body_outer_h = Bottom_Height + Wall_Thickness;
ridge_z_bot = body_outer_h - 2;
// Lid tab in lid LOCAL coords: Z=$b_top_outer_height (mating face) extending
// snap_tab_drop mm beyond. After mirror+translate, this lands ON TOP of the
// body's outside front wall, overlapping it.
lid_outer_h = Top_Height + Wall_Thickness;
tab_z_top_local = lid_outer_h;
tab_z_bot_local = lid_outer_h + snap_tab_drop;
// Notch in lid LOCAL coords positioned so that, after mirror+translate to
// assembled coords, it sits exactly over the body bump.
// Mapping: assembled_z = body_outer_h + lid_outer_h - lid_local_z
notch_z_center_local = body_outer_h + lid_outer_h - (ridge_z_bot + snap_ridge_h / 2);
notch_z_bot_local    = notch_z_center_local - snap_notch_h / 2;

module __end_customizer_options__() { }

// USB-C cutout — pierce ONE short wall at the board center (LEFT side wall).
module usbc_cutouts() {
    translate([-(wall_x + 2), board_box_y - usbc_opening_w / 2, usbc_z - usbc_opening_h / 2])
        cube([cut_depth, usbc_opening_w, usbc_opening_h]);
}

// Lid grills — grid of small square holes (waffle pattern) cut through the
// lid. Disabled by default (grill_cols/grill_rows = 0); heat relief comes
// from heat_relief_holes() instead.
module lid_grills() {
    pitch = grill_sq + grill_gap;
    bank_x = grill_cols * grill_sq + (grill_cols - 1) * grill_gap;
    bank_y = grill_rows * grill_sq + (grill_rows - 1) * grill_gap;
    x0 = -bank_x / 2;
    y0 = -bank_y / 2;
    for (i = [0 : grill_cols - 1])
        for (j = [0 : grill_rows - 1])
            translate([x0 + i * pitch, y0 + j * pitch, -1])
                cube([grill_sq, grill_sq, Wall_Thickness + 2]);
}

// Bare-board placement / supports
//
// One SuperMini, LONG axis (board_w_x) aligned with box X, centered at
// box Y = 0. PCB corners:
//   X = ±board_w_x/2
//   Y = ±board_l_y/2
// Posts inset 1 mm in from PCB corners for stability. The board's LONG
// axis (with USB-C end) points in X — out through the X-wall cutout.
floor_post_h     = 2;            // PCB sits this high above the interior floor
post_d           = 3;            // post diameter
post_inset       = 1;            // post inset from PCB corner
pcb_top_z        = Wall_Thickness + floor_post_h + pcb_thickness;
// Lid posts press 1 mm into the PCB position so they reliably hold the
// board even with print-tolerance variance (±0.3 mm in Z is common). The
// PCB and posts compress slightly — the lid still closes flush. If yours
// doesn't close, drop this to 0.5 or 0.3.
lid_post_interference = 1.0;
lid_post_z_bot   = body_outer_h + lid_outer_h - (pcb_top_z - lid_post_interference);
lid_post_len     = lid_post_z_bot - Wall_Thickness;

// Compute post X,Y positions for the single board (4 corners)
function _board_post_xy(board_y_center) = [
    [board_box_x - (board_w_x/2 - post_inset), board_y_center - (board_l_y/2 - post_inset)],
    [board_box_x - (board_w_x/2 - post_inset), board_y_center + (board_l_y/2 - post_inset)],
    [board_box_x + (board_w_x/2 - post_inset), board_y_center - (board_l_y/2 - post_inset)],
    [board_box_x + (board_w_x/2 - post_inset), board_y_center + (board_l_y/2 - post_inset)],
];
all_post_xy = _board_post_xy(board_box_y);

// 4 small floor posts at PCB corners — PCB rests on top
module floor_posts() {
    for (xy = all_post_xy)
        translate([xy[0], xy[1], Wall_Thickness])
            cylinder(d = post_d, h = floor_post_h);
}

// 4 small lid posts matching the floor posts in X,Y. When the lid closes,
// these press DOWN on the PCB corners to keep the board seated.
module lid_posts() {
    for (xy = all_post_xy)
        translate([xy[0], xy[1], Wall_Thickness])
            cylinder(d = post_d, h = lid_post_len);
}

// Back constraint rib — small inner-wall protrusion that locks the BACK of
// the board in X with 0.3 mm of clearance. The PCB drops in through the
// wider lip-Z opening, descends to PCB Z (where the wall is thinner and
// would otherwise leave slop), and the rib material there extends the
// inner wall back to a snug fit, only at the back-of-board Y range. The
// front (USB-C side) is held by the cable. Board USB-C is on -X, so the
// back of the board is on +X — put the rib on the +X wall.
back_rib_clearance = 0.3;    // gap between rib inner face and PCB back edge
back_rib_y_half = 5;         // half-extent in Y (10 mm wide rib)

module back_ribs() {
    rib_x_inner = board_box_x + board_w_x / 2 + back_rib_clearance;
    // Extend OUTWARD into wall material at PCB Z so the rib is structurally
    // anchored to the wall by CSG union.
    rib_x_outer = Width / 2 + 2;
    rib_thickness_x = rib_x_outer - rib_x_inner;
    // Rib Z: from interior floor up to slightly above PCB top. Don't extend
    // into the lip area (already wall material there; could foul lid posts).
    rib_z_min = Wall_Thickness;
    rib_z_max = pcb_top_z + 0.5;
    rib_z_height = rib_z_max - rib_z_min;

    // Back rib on +X wall (board USB-C on -X → back on +X)
    translate([rib_x_inner, board_box_y - back_rib_y_half, rib_z_min])
        cube([rib_thickness_x, back_rib_y_half * 2, rib_z_height]);
}

// Body snap-fit ridge — small horizontal bump on outside front wall
module body_snap_ridge() {
    translate([
        -snap_w / 2,
        front_wall_y - snap_protrusion,
        ridge_z_bot
    ])
        cube([snap_w, snap_protrusion, snap_ridge_h]);
}

// Lid snap-fit tab — tapered + sized ANCHOR BLOCK.
// The anchor is sized to the THINNEST lid-wall depth (just Wall_Thickness)
// and stops AT the mating face, so it never intrudes into the lid inner
// cavity or the body cavity. Volumetric overlap with the tab is kept by
// extending the tab's top UP by 1 mm into the anchor area at the outer Y.
// Defined in lid LOCAL coords; rbox_for_top() handles the mirror.
module lid_snap_tab() {
    eps = 0.4;
    anchor_y_depth = snap_tab_thick_root + Wall_Thickness;
    union() {
        // Anchor block — outer portion of the lid wall only.
        translate([
            -snap_w / 2,
            front_wall_y - snap_tab_thick_root,
            0
        ])
            cube([snap_w, anchor_y_depth, tab_z_top_local]);

        // Tapered tab below the mating face — extended UP into the anchor by
        // 1 mm at the outer Y range for volumetric (not just 2D) continuity.
        hull() {
            translate([
                -snap_w / 2,
                front_wall_y - snap_tab_thick_root,
                tab_z_top_local - 1
            ])
                cube([snap_w, snap_tab_thick_root, eps]);
            translate([
                -snap_w / 2,
                front_wall_y - snap_tab_thick_root,
                tab_z_top_local
            ])
                cube([snap_w, snap_tab_thick_root, eps]);
            translate([
                -snap_w / 2,
                front_wall_y - snap_tab_thick_tip,
                tab_z_top_local + snap_tab_drop - eps
            ])
                cube([snap_w, snap_tab_thick_tip, eps]);
        }
    }
}

// Lid snap-fit notch — captures the body ridge when fully closed.
// (Subtracted from the lid+tab; lid local coords.) Slightly oversized.
module lid_snap_notch() {
    translate([
        -snap_w / 2 - 0.5,
        front_wall_y - snap_protrusion - 0.1,
        notch_z_bot_local
    ])
        cube([snap_w + 1, snap_protrusion + 0.2, snap_notch_h]);
}

// Heat-relief holes — oval pattern of 1.9 mm holes across the lid top face.
// Generated on a 3 mm grid, filtered to an ellipse, avoiding lid posts.
// Ellipse semi-axes are auto-derived from the box interior so the pattern
// scales with the (possibly resized) box.
heat_hole_d = 1.9;
heat_ellipse_sx = Width / 2 - 4;     // semi-axis X (4 mm margin from inner wall)
heat_ellipse_sy = Length / 2 - 3;    // semi-axis Y (3 mm margin from inner wall)
heat_post_clearance = 2.5;           // min distance from hole center to any post center

module heat_relief_holes() {
    for (gx = [-ceil(heat_ellipse_sx) : 3 : ceil(heat_ellipse_sx)])
        for (gy = [-ceil(heat_ellipse_sy) : 3 : ceil(heat_ellipse_sy)])
            if ((gx/heat_ellipse_sx)*(gx/heat_ellipse_sx) +
                (gy/heat_ellipse_sy)*(gy/heat_ellipse_sy) <= 1)
                if (_heat_hole_ok(gx, gy))
                    translate([gx, gy, -1])
                        cylinder(d = heat_hole_d, h = Wall_Thickness + 2, $fn = 16);
}

function _heat_hole_ok(hx, hy) =
    min([for (xy = all_post_xy)
        sqrt((hx - xy[0])*(hx - xy[0]) + (hy - xy[1])*(hy - xy[1]))
    ]) > heat_post_clearance;

// Bottom label — recess text into the outside bottom face.
// rbox_for_bottom() local coords: Z=0 is outside bottom face, Z=wall is
// interior. Cut from Z=-1 up to Z=Bottom_Label_Depth (recess, not pierced).
// mirror([1,0,0]) pre-flips X so glyphs read CORRECTLY when viewed from BELOW.
module bottom_label() {
    translate([0, 0, -1])
        linear_extrude(height = 1 + Bottom_Label_Depth)
        mirror([1, 0, 0])
        text(
            Bottom_Label_Text,
            size = Bottom_Label_Size,
            font = "Helvetica:style=Bold",
            halign = "center",
            valign = "center"
        );
}

// Render
//   children(0) = bottom body − USB-C cutout − bottom-label engraving
//   children(1) = top body − grills − heat holes − snap notch
rbox(
    Width, Length, Bottom_Height, Top_Height,
    corner_radius = Corner_Radius,
    lip_seal_type = "wedge",
    latch_type = "draw",          // printed-pin hinge, no screw needed
    handle = false,
    label = false
)
rbox_size_adjustments(
    wall_thickness = Wall_Thickness,
    lip_thickness = Lip_Thickness,
    latch_width = Latch_Width,
    latch_screw_separation = Latch_Screw_Separation
)
rbox_part(Part) {
    // Child 0 — bottom: add snap ridge + floor posts + back rib, subtract USB-C cutout + label
    difference() {
        union() {
            rbox_body();
            body_snap_ridge();
            floor_posts();
            back_ribs();
        }
        usbc_cutouts();
        bottom_label();
    }
    // Child 1 — top: add snap tab + lid posts, subtract grills + heat holes + snap notch
    difference() {
        union() {
            rbox_body();
            lid_snap_tab();
            lid_posts();
        }
        lid_grills();
        heat_relief_holes();
        lid_snap_notch();
    }
}
