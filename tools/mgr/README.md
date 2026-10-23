Taken from https://github.com/tuancapoo/mgr-nx-deko (GPL-3.0), tools/:
- guest_cas.py (the run-time barrier switch removed)
- vec_byteswap.py
- pch_neon_shuffles.py
Post-codegen passes over the generated C++. Prototype runs them (prototype/build.sh); for Prototype 2 they made
the picture softer and the frame rate no better, so its build does not use them yet.
