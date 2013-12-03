ALTER TABLE build MODIFY project VARCHAR(64);
CREATE INDEX build_project ON build (project);
