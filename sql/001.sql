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
