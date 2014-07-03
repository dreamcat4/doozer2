ALTER TABLE build ADD COLUMN buildenv VARCHAR(64);
CREATE INDEX build_buildenv ON build (buildenv);
