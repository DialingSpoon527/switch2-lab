#!/usr/bin/env python3
"""Static regressions for the separated updater. Does not imply hardware safety."""
from pathlib import Path
import re
import hashlib
root = Path(__file__).resolve().parents[2]
controller = (root / 'hotupdate-controller/source/main.cpp').read_text()
monitor = (root / 'hotupdate-supervisor/source/deferred_monitor.cpp').read_text()
supervisor = (root / 'hotupdate-supervisor/source/main.cpp').read_text()
launch = (root / 'hotupdate-supervisor/source/launch_backend.cpp').read_text()
core = (root / 'hotupdate-supervisor/source/restart_controller.cpp').read_text()
makefile = (root / 'hotupdate-supervisor/Makefile').read_text()

assert 'replaceRo(' not in core, 'Native RO process replacement must not be compiled'
assert 'replaceRo(' not in monitor, 'RO live transaction must be unreachable'
assert 'DeferredQueueRoCommand || command == DeferredPreflightRoCommand' in supervisor, 'RO IPC must be denied explicitly'
assert 'if (!hen_path) return Invalid;' in launch, 'Launcher must accept HEN staging only'
assert 'if (!hen_path && !ro_path' not in launch
assert 'ro_flight.o' not in makefile and 'ro_trace.o' not in makefile
assert 'ro/source/wire.h' in core, 'HEN still requires existing RO handoff commands'
assert 'Request::QueueHen' in controller
assert 'Request::ApplyManifest' in controller
assert 'fsFsDeleteFile(&sd, RequestPath)' in controller
assert controller.index('fsFsDeleteFile(&sd, RequestPath)') < controller.index('readPackage(&sd, package_digest)'), 'One-shot command must be consumed first'
assert '/kaihen/hotupdate/hen-command.txt' in controller
assert '/kaihen/hotupdate/hen-package.txt' in controller
assert '/kaihen/hotupdate/hen-result.txt' in controller
assert 'hotupdate-supervisor' in controller, 'Bootstrap escrow requires exact plugin name'

# The V11 body was changed deliberately: stop-reply uncertainty now disables
# rollback, and an unresolved launcher cannot resume the old gate owner.
# Behavioral fault-injection coverage lives in tools/test_transactions.py.
start = core.index('Result Controller::replaceHen(')
end = core.index('} // namespace hotupdate', start)
sha = hashlib.sha256(core[start:end].encode()).hexdigest()
body = core[start:end]
stop = body.index('rc = henStopIfHeld(&previous);')
assert body.rfind('report->rollback_available = 0;', 0, stop) > 0
assert 'if (R_FAILED(rc)) goto failed_after_stop;' in body[stop:stop+120]
assert 'DeferredQueueHenPackageCommand' in controller
assert 'const DeferredHenPackageRequest* expected' in monitor
print('HEN updater isolated-path regression checks: PASS')
print('HEN native replacement function SHA256:', sha)
