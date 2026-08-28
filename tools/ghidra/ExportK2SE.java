// Export named functions from a Ghidra project to the CSV that
// tools/import_ghidra_csv.py reads.
//
// Naming: prefer Ghidra's real namespace (Class::Function), and fall back to a
// flat Class__Function name for functions parked in the Global namespace. Free
// functions get class "Other", which is the sentinel the address table uses.
//
// Output columns:
//     class_name,function_name,address,calling_convention,param_size_bytes,notes
//
// param_size_bytes is the caller-visible argument size -- what a __stdcall or
// __thiscall callee pops on the way out. It is the single most useful thing to
// carry across, because calling a callee-cleaned function with the wrong
// argument count unbalances the stack silently rather than failing.
//
// Runs either from the Script Manager or headless:
//     analyzeHeadless <proj> <name> -process swkotor2.exe \
//         -postScript ExportK2SE.java C:\path\to\functions.csv
//
// @category KOTOR
// @menupath Tools.KOTOR.Export K2SE functions CSV

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportK2SE extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = (args.length > 0) ? new File(args[0])
                                     : askFile("Export K2SE functions CSV", "Save");

        int exported = 0;
        int skipped = 0;

        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            w.println("class_name,function_name,address,calling_convention,"
                    + "param_size_bytes,notes");

            for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
                // Only deliberately-named functions. An auto-named one carries no
                // information a CSV row could preserve.
                if (fn.getSymbol().getSource() == SourceType.DEFAULT) {
                    skipped++;
                    continue;
                }
                String name = fn.getName();
                if (name.startsWith("FUN_") || name.startsWith("thunk_")) {
                    skipped++;
                    continue;
                }

                String className = "Other";
                String functionName = name;

                Namespace ns = fn.getParentNamespace();
                if (ns != null && !ns.isGlobal()) {
                    className = ns.getName();
                } else {
                    // Flat Class__Function convention, split on the LAST "__" so
                    // that a function name containing "__" still parses.
                    int sep = name.lastIndexOf("__");
                    if (sep > 0) {
                        className = name.substring(0, sep);
                        functionName = name.substring(sep + 2);
                    }
                }

                String cc = fn.getCallingConventionName();
                if (cc == null || cc.equals("unknown") || cc.equals("default")) {
                    cc = "";
                }

                // Each parameter occupies at least one 4-byte stack slot on x86.
                // An empty field means "no parameters recorded", which is not the
                // same as a recorded zero -- the importer keeps them distinct.
                int paramBytes = 0;
                boolean hasParams = false;
                for (Parameter p : fn.getParameters()) {
                    int len = p.getDataType().getLength();
                    if (len < 1) {
                        len = 4;
                    }
                    paramBytes += ((len + 3) / 4) * 4;
                    hasParams = true;
                }

                w.println(csv(className) + "," + csv(functionName) + ","
                        + String.format("0x%08X", fn.getEntryPoint().getOffset()) + ","
                        + csv(cc) + "," + (hasParams ? String.valueOf(paramBytes) : "")
                        + "," + csv(fn.getComment()));
                exported++;
            }
        }

        println("Exported " + exported + " functions (" + skipped + " unnamed skipped) to "
                + out.getAbsolutePath());
    }

    // RFC-4180, with one deliberate deviation: newlines are flattened to spaces
    // rather than quoted. A quoted newline is legal CSV but splits the record
    // across physical lines, and every line-oriented reader downstream then
    // mis-parses it. Plate comments are free text and routinely multi-line, so
    // this is a real hazard rather than a theoretical one. Nothing of value is
    // lost by collapsing them.
    private String csv(String s) {
        if (s == null) {
            return "";
        }
        s = s.replace("\r\n", " ").replace('\n', ' ').replace('\r', ' ');
        if (s.indexOf(',') >= 0 || s.indexOf('"') >= 0) {
            return "\"" + s.replace("\"", "\"\"") + "\"";
        }
        return s;
    }
}
