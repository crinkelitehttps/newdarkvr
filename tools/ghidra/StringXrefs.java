// Ghidra headless helper. Args: <outfile> <regex>...
// For every defined string whose text matches any regex (Java find(), case as written, use (?i) for
// case-insensitive), write its address, its text, and each reference to it with the containing function.
// The binary has no symbols, so strings are how you find engine code: run this, then feed the function
// addresses to DecompileAt.java. Addresses match objdump's (image base 0x400000).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

public class StringXrefs extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("usage: StringXrefs.java <outfile> <regex>...");
			return;
		}
		List<Pattern> pats = new ArrayList<>();
		for (int i = 1; i < args.length; i++) {
			pats.add(Pattern.compile(args[i]));
		}
		int hits = 0;
		try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
			out.println("// ==== StringXrefs " + String.join(" | ", java.util.Arrays.copyOfRange(args, 1, args.length)) + " ====");
			DataIterator it = currentProgram.getListing().getDefinedData(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Data d = it.next();
				if (!StringDataInstance.isString(d)) {
					continue;
				}
				String s = StringDataInstance.getStringDataInstance(d).getStringValue();
				if (s == null) {
					continue;
				}
				boolean match = false;
				for (Pattern p : pats) {
					if (p.matcher(s).find()) {
						match = true;
						break;
					}
				}
				if (!match) {
					continue;
				}
				hits++;
				String shown = s.replace("\n", "\\n");
				out.println(d.getAddress() + "  \"" + (shown.length() > 100 ? shown.substring(0, 100) + "..." : shown) + "\"");
				for (Reference r : getReferencesTo(d.getAddress())) {
					Function f = getFunctionContaining(r.getFromAddress());
					out.println("    <- " + r.getFromAddress() + " in "
						+ (f == null ? "<no function>" : f.getName() + "@" + f.getEntryPoint()));
				}
			}
			out.println("// " + hits + " matching strings");
		}
	}
}
