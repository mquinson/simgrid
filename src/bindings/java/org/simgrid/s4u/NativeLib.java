/* Copyright (c) 2014-2024. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

package org.simgrid.s4u;

import java.io.InputStream;
import java.io.IOException;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.stream.Stream;

/** Helper class loading the native functions of SimGrid that we use for downcalls
 *
 * Almost all org.simgrid.msg.* classes contain a static block (thus executed when the class is loaded)
 * containing a call to this.
 */
public final class NativeLib {
	private static final boolean WINDOWS_OS = System.getProperty("os.name").toLowerCase().startsWith("win");
	private static boolean isNativeInited = false;
	private static Path tempDir = null; // where the embeeded libraries are unpacked before loading them

	/** A static-only "class" don't need no constructor */
	private NativeLib() {
		throw new IllegalAccessError("Utility class");
	}

	/** Hidden debug main() function
	 *
	 * It is not the Main-Class defined in src/bindings/java/MANIFEST.in (org.simgrid.msg.Msg is),
	 * so it won't get executed by default. But that's helpful to debug linkage errors, if you
	 * know that it exists. It's used by cmake during the configure, to inform the user.
	 */
	public static void main(String[] args) {
		System.out.println("This jarfile searches the native code under: " +getPath());
	}

	/** Main function loading all the native classes that we need */
	public static void nativeInit() {
		if (isNativeInited)
			return;

		if (WINDOWS_OS)
			NativeLib.nativeInit("winpthread-1");

		NativeLib.nativeInit("simgrid");
		NativeLib.nativeInit("simgrid-java");
		isNativeInited = true;

                /* Don't leak the files on disk */
                if (tempDir != null) {
                  FileCleaner fclean = new FileCleaner(tempDir.toFile());
                  fclean.run();
                }
	}

	/** Helper function trying to load one requested library */
	public static void nativeInit(String name) {
		Throwable cause = null;
		try {
			/* Prefer the version of the library bundled into the jar file and use it */
			if (loadLibAsStream(name))
				return;
		} catch (UnsatisfiedLinkError|SecurityException|IOException e) {
			cause = e;
		}

		/* If not found, try to see if we can find a version on disk */
		try {
			System.loadLibrary(name);
			return;
                } catch (UnsatisfiedLinkError systemException) {
                  System.err.println(systemException);
                }

                System.err.println("\nCannot load the bindings to the '" + name + "'' library in path " + getPath() +
                                   " and no usable SimGrid installation found on disk.");
                if (cause != null) {
                  if (cause.getMessage().contains("libcgraph.so"))
                    System.err.println("HINT: Try to install the libcgraph package (sudo apt-get install libcgraph).");
                  else if (cause.getMessage().contains("libboost_context.so"))
                    System.err.println(
                        "HINT: Try to install the boost-context package (sudo apt-get install libboost-context-dev).");
                  else
                    System.err.println(
                        "Try to install the missing dependencies, if any. Read carefully the following error message.");

                  System.err.println();
                  cause.printStackTrace();
                } else {
                  System.err.println("This jar file does not seem to fit your system, and no usable SimGrid "
                                     + "installation found on disk for " + name + ".");
                }
                System.exit(1);
        }

	/** Try to extract the library from the jarfile before loading it */
	private static boolean loadLibAsStream (String name) throws IOException, UnsatisfiedLinkError {
		String path = NativeLib.getPath();

		// We must write the lib onto the disk before loading it -- stupid operating systems
		if (tempDir == null) {
			final String tempPrefix = "simgrid-java-";

			if (WINDOWS_OS) {
				// The cleanup at exit fails on Windows where it is impossible to delete files which are still in
				// use.  Try to remove stale temporary files from previous executions, and limit disk usage.
				Path tmpdir = (new File(System.getProperty("java.io.tmpdir"))).toPath();
				try (Stream<Path> paths = Files.find(tmpdir, 1,
					(Path p, java.nio.file.attribute.BasicFileAttributes a) ->
						a.isDirectory() && !p.equals(tmpdir) &&
						p.getFileName().toString().startsWith(tempPrefix))) {
					paths.map(Path::toFile)
					     .map(FileCleaner::new)
					     .forEach(FileCleaner::run);

				}
			}

			tempDir = Files.createTempDirectory(tempPrefix);
		}

		/* For each possible filename of the given library on all possible OSes, try it */
		for (String filename : new String[]
		   { name,
		     "lib"+name+".so",               /* linux */
		     name+".dll", "lib"+name+".dll", /* windows (pure and mingw) */
		     "lib"+name+".dylib"             /* macOS */}) {

			File fileOut = new File(tempDir.toFile(), filename);
			try ( // Try-with-resources. These stream will be autoclosed when needed.
				InputStream in = NativeLib.class.getClassLoader().getResourceAsStream(path+filename);
			) {
				if (in != null) {
					/* copy the library in position */
					Files.copy(in, fileOut.toPath());

					/* load that library */
					System.load(fileOut.getAbsolutePath());

					/* It loaded! we're good */
					return true;
				}
			}
		}

		/* No suitable name found */
		return false;
	}

	/** Find where to search for the library in the jar -- keep it aligned with where cmake puts it! */
	private static String getPath() {
		// Inspiration: https://github.com/xerial/snappy-java/blob/develop/src/main/java/org/xerial/snappy/OSInfo.java
		String prefix = "NATIVE";
		String os = System.getProperty("os.name");
		String arch = System.getProperty("os.arch");

		if (arch.matches("^i[3-6]86$"))
			arch = "x86";
		else if ("x86_64".equalsIgnoreCase(arch) || "AMD64".equalsIgnoreCase(arch))
			arch = "amd64";

		if (os.toLowerCase().startsWith("win")) {
			os = "Windows";
		} else if (os.contains("OS X")) {
			os = "Darwin";
		}
		os = os.replace(' ', '_');
		arch = arch.replace(' ', '_');

		return prefix + "/" + os + "/" + arch + "/";
	}

	/** A hackish mechanism used to remove the file containing our library when the JVM shuts down */
	private static class FileCleaner implements Runnable {
		private File dir;
		public FileCleaner(File dir) {
			this.dir = dir;
		}
		@Override
		public void run() {
                        try (Stream<Path> paths = Files.walk(dir.toPath())) {
                                paths.sorted(java.util.Comparator.reverseOrder())
                                     .map(java.nio.file.Path::toFile)
                                     //.peek(System.err::println) // Prints what gets removed
                                     .forEach(java.io.File::delete);
			} catch(Exception e) {
				System.err.println("Error while cleaning temporary file " + dir.getAbsolutePath() +
				                   ": " + e.getCause());
				e.printStackTrace();
                        }
		}
	}
}
