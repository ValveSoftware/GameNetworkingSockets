<?php
	function echoln($str) {
		echo $str;
		echo "\n";
	}

	function usage($reason) {
		echoln("Usage: php build-nix.php [flags]");
		echoln("Flags in parantheses are optional");
		echoln("");
		echoln("  --bits=[32,64]");
		echoln("  --function=[curve25519,ed25519]");
		echoln(" (--compiler=[*gcc,clang,icc])        which compiler to use, gcc is default");
		echoln(" (--with-openssl)                     use openssl for SHA512");
		echoln(" (--with-sse2)                        additionally fuzz against SSE2");
		echoln(" (--no-asm)                           don't use platform specific asm");
		echoln("");
		if ($reason)
			echoln($reason);
	}

	function cleanup() {
		system("rm -f *.o");
	}

	function runcmd($desc, $cmd) {
		echoln($desc);

		$ret = 0;
		system($cmd, $ret);
		if ($ret) {
			cleanup();
			exit;
		}
	}

	class argument {
		var $set, $value;
	}

	class multiargument extends argument {
		function multiargument($flag, $legal_values) {
			global $argc, $argv;

			$this->set = false;

			$map = array();
			foreach($legal_values as $value)
				$map[$value] = true;

			for ($i = 1; $i < $argc; $i++) {
				if (!preg_match("!--".$flag."=(.*)!", $argv[$i], $m))
					continue;
				if (isset($map[$m[1]])) {
					$this->value = $m[1];
					$this->set = true;
					return;
				} else {
					usage("{$m[1]} is not a valid parameter to --{$flag}!");
					exit(1);
				}
			}
		}
	}

	class flag extends argument {
		function flag($flag) {
			global $argc, $argv;

			$this->set = false;

			$flag = "--{$flag}";
			for ($i = 1; $i < $argc; $i++) {
				if ($argv[$i] !== $flag)
					continue;
				$this->value = true;
				$this->set = true;
				return;
			}
		}
	}

	$bits = new multiargument("bits", array("32", "64"));
	$function = new multiargument("function", array("curve25519", "ed25519"));
	$compiler = new multiargument("compiler", array("gcc", "clang", "icc"));
	$with_sse2 = new flag("with-sse2");
	$with_openssl = new flag("with-openssl");
	$no_asm = new flag("no-asm");

	$err = "";
	if (!$bits->set)
		$err .= "--bits not set\n";
	if (!$function->set)
		$err .= "--function not set\n";

	if ($err !== "") {
		usage($err);
		exit;
	}

	$compile = ($compiler->set) ? $compiler->value : "gcc";
	$link = "";
	$flags = "-O3 -m{$bits->value}";
	$ret = 0;

	if ($with_openssl->set) $link .= " -lssl -lcrypto";
	if (!$with_openssl->set) $flags .= " -DED25519_REFHASH -DED25519_TEST";
	if ($no_asm->set) $flags .= " -DED25519_NO_INLINE_ASM";

	if ($function->value === "curve25519") {
		runcmd("building ref10..", "{$compile} {$flags} curve25519-ref10.c -c -o curve25519-ref10.o");
		runcmd("building ed25519..", "{$compile} {$flags} ed25519-donna.c -c -o ed25519.o");
		if ($with_sse2->set) {
			runcmd("building ed25519-sse2..", "{$compile} {$flags} ed25519-donna-sse2.c -c -o ed25519-sse2.o -msse2");
			$flags .= " -DED25519_SSE2";
			$link .= " ed25519-sse2.o";
		}
		runcmd("linking..", "{$compile} {$flags} {$link} fuzz-curve25519.c ed25519.o curve25519-ref10.o -o fuzz-curve25519");
		echoln("fuzz-curve25519 built.");
	} else 	if ($function->value === "ed25519") {
		runcmd("building ref10..", "{$compile} {$flags} ed25519-ref10.c -c -o ed25519-ref10.o");
		runcmd("building ed25519..", "{$compile} {$flags} ed25519-donna.c -c -o ed25519.o");
		if ($with_sse2->set) {
			runcmd("building ed25519-sse2..", "{$compile} {$flags} ed25519-donna-sse2.c -c -o ed25519-sse2.o -msse2");
			$flags .= " -DED25519_SSE2";
			$link .= " ed25519-sse2.o";
		}
		runcmd("linking..", "{$compile} {$flags} {$link} fuzz-ed25519.c ed25519.o ed25519-ref10.o -o fuzz-ed25519");
		echoln("fuzz-ed25519 built.");
	}


	cleanup();
?>
