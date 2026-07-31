// verify.jsx — After Effects verification for Specularity.
// Run in AE: File > Scripts > Run Script File… > select this file.
// It creates a comp, applies the "Specularity" effect by match name, toggles a param,
// and reports whether the native plugin loaded and applied successfully.
(function () {
    var MATCH_NAME = "com.cosmo88.ae.Specularity";
    try {
        app.beginUndoGroup("Specularity verify");

        var comp = app.project.items.addComp("SpecularityVerify", 256, 256, 1.0, 1.0, 24);
        var solid = comp.layers.addSolid([0.5, 0.5, 1.0], "normal-map", 256, 256, 1.0); // flat normal color

        var fx = solid.property("ADBE Effect Parade");
        if (!fx.canAddProperty(MATCH_NAME)) {
            alert("FAIL: After Effects cannot add effect '" + MATCH_NAME + "'.\n" +
                  "The plugin is not installed or not recognized.");
            app.endUndoGroup();
            return;
        }
        var eff = fx.addProperty(MATCH_NAME);

        // Poke the Display popup (param index 1) to confirm params are live.
        var info = "Effect applied: " + eff.name + "\n";
        info += "Params: " + eff.numProperties + "\n";
        try { eff.property(1).setValue(1); info += "Display set OK\n"; } catch (e) { info += "Display set: " + e + "\n"; }

        alert("PASS OK  Specularity loaded and applied natively.\n\n" + info +
              "\nArchitecture: " + (system.osName || "macOS") +
              "\nAE " + app.version);

        app.endUndoGroup();
    } catch (err) {
        alert("ERROR during verify: " + err.toString());
    }
})();
