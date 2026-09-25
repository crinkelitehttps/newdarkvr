// Ghidra headless helper. Args: <outfile> <spec>...
//   spec = <hexaddr>    decompile the function containing/at that address (creates it if none)
//   spec = x:<hexaddr>  list all references to that address (code or data) with containing function
// Addresses are the same as objdump's (image base 0x400000).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAt extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("usage: DecompileAt.java <outfile> <hexaddr | x:hexaddr>...");
			return;
		}
		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		try (PrintWriter out = new PrintWriter(new FileWriter(args[0], true))) {
			for (int i = 1; i < args.length; i++) {
				String spec = args[i];
				boolean xrefs = spec.startsWith("x:");
				Address a = toAddr(xrefs ? spec.substring(2) : spec);
				if (xrefs) {
					out.println("// ==== references to " + a + " ====");
					for (Reference r : getReferencesTo(a)) {
						Function f = getFunctionContaining(r.getFromAddress());
						out.println("//   " + r.getFromAddress() + " " + r.getReferenceType() + " in "
							+ (f == null ? "<no function>" : f.getName() + "@" + f.getEntryPoint()));
					}
					continue;
				}
				Function f = getFunctionContaining(a);
				if (f == null) {
					disassemble(a);
					f = createFunction(a, null);
				}
				if (f == null) {
					out.println("// ==== " + spec + ": no function ====");
					continue;
				}
				out.println("// ==== " + f.getName() + " @ " + f.getEntryPoint() + " (requested " + spec + ") ====");
				out.println("// callers:");
				for (Reference r : getReferencesTo(f.getEntryPoint())) {
					Function c = getFunctionContaining(r.getFromAddress());
					out.println("//   " + r.getFromAddress() + " in "
						+ (c == null ? "<no function>" : c.getName() + "@" + c.getEntryPoint()));
				}
				DecompileResults res = ifc.decompileFunction(f, 90, monitor);
				if (res.decompileCompleted()) {
					out.println(res.getDecompiledFunction().getC());
				} else {
					out.println("// decompile failed: " + res.getErrorMessage());
				}
			}
		}
	}
}
