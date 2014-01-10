CREATE TABLE deleted_artifact (
       id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
       name TEXT NOT NULL,
       storage VARCHAR(16) NOT NULL,
       payload TEXT,
       project TEXT NOT NULL,
       error TEXT
)  ENGINE InnoDB;


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

DELIMITER |
CREATE TRIGGER build_delete_trigger BEFORE DELETE ON build
  FOR EACH ROW
   BEGIN
     INSERT INTO deleted_artifact (name, storage, payload, project) SELECT name, storage, payload, OLD.project FROM artifact WHERE build_id = OLD.id AND storage != "embedded";
  END;
|

DELIMITER ;
