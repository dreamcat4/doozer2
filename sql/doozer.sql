DROP TABLE artifact;
DROP TABLE build;

CREATE TABLE build (
       id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
       created TIMESTAMP DEFAULT NOW(),
       project TEXT NOT NULL,
       revision VARCHAR(40) NOT NULL,
       branch TEXT,
       version TEXT,
       target  VARCHAR(64) NOT NULL,
       type TEXT NOT NULL,
       status VARCHAR(32),
       status_change TIMESTAMP,
       buildstart TIMESTAMP,
       buildend TIMESTAMP,
       attempts INT DEFAULT 0,
       agent TEXT,
       progress_text TEXT,
       jobsecret VARCHAR(32)) ENGINE InnoDB; 


CREATE INDEX build_target ON build (target);
CREATE INDEX build_created ON build (created);
CREATE INDEX build_status ON build (status);

CREATE TABLE artifact (
       id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
       build_id INT NOT NULL,
       created TIMESTAMP DEFAULT NOW(),
       name TEXT NOT NULL,
       type TEXT NOT NULL,
       storage VARCHAR(16) NOT NULL,
       payload TEXT,
       size INT NOT NULL,
       md5 VARCHAR(32),
       sha1 VARCHAR(40),
       INDEX build_id_ind (build_id),
       FOREIGN KEY (build_id) REFERENCES build(id) ON DELETE CASCADE
       ) ENGINE InnoDB;


-- 002

ALTER TABLE artifact ADD COLUMN dlcount INT DEFAULT 0;

-- 003

ALTER TABLE build ADD COLUMN no_output BOOL DEFAULT false;

-- 004

ALTER TABLE artifact ADD COLUMN contenttype TEXT;

-- 005

drop table deleted_artifact;
CREATE TABLE deleted_artifact (
       id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
       name TEXT NOT NULL,
       storage VARCHAR(16) NOT NULL,
       payload TEXT,
       project TEXT NOT NULL,
       error TEXT
)  ENGINE InnoDB;

DROP TRIGGER artifact_delete_trigger;

DELIMITER |

CREATE TRIGGER artifact_delete_trigger BEFORE DELETE ON artifact
  FOR EACH ROW
   BEGIN
     IF OLD.storage != "embedded" THEN
       INSERT INTO deleted_artifact (name, storage, payload, project) VALUES (OLD.name, OLD.storage, OLD.payload, (SELECT project FROM build WHERE id = OLD.build_id));
     END IF;
  END;
|
DELIMITER ;

DROP TRIGGER build_delete_trigger;
DELIMITER |
CREATE TRIGGER build_delete_trigger BEFORE DELETE ON build
  FOR EACH ROW
   BEGIN
     INSERT INTO deleted_artifact (name, storage, payload, project) SELECT name, storage, payload, OLD.project FROM artifact WHERE build_id = OLD.id AND storage != "embedded";
  END;
|

DELIMITER ;

-- 006

ALTER TABLE artifact ADD COLUMN encoding TEXT;

--- 007

CREATE INDEX artifact_sha1 ON artifact (sha1);

----

SHOW COLUMNS FROM build;


