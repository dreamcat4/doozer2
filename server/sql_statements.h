#pragma once

#define SQL_GET_ARTIFACT_BY_SHA1 "SELECT storage,payload,project,name,artifact.type,contenttype,encoding FROM artifact,build WHERE artifact.sha1=? AND build.id = artifact.build_id"

#define SQL_INCREASE_DLCOUNT_BY_SHA1 "UPDATE artifact SET dlcount = dlcount + 1 WHERE sha1 = ?"

#define SQL_INCREASE_PATCHCOUNT_BY_SHA1 "UPDATE artifact SET patchcount = patchcount + 1 WHERE sha1 = ?"

#define SQL_GET_TARGETS_FOR_BUILD "SELECT target,id,status FROM build WHERE revision = ? AND project = ?"

#define SQL_INSERT_BUILD "INSERT INTO build (project,revision,target,type,status,version,no_output) VALUES (?,?,?,?,?,?,?)"

#define SQL_ALLOC_BUILD "UPDATE build SET agent=?, status=?, status_change=NOW(), buildstart=NOW(), attempts = attempts + 1, jobsecret=? WHERE id=?"

#define SQL_GET_BUILD_BY_ID "SELECT project,revision,target,type,agent,jobsecret,status,version FROM build WHERE id=?"

#define SQL_INSERT_ARTIFACT "INSERT INTO artifact (build_id, type, payload, storage, name, size, md5, sha1, contenttype, encoding, origsize) VALUES (?,?,?,?,?,?,?,?,?,?,?)"

#define SQL_BUILD_PROGRESS_UPDATE "UPDATE build SET progress_text=?,status_change=NOW() WHERE id=?"

#define SQL_BUILD_FINISHED "UPDATE build SET status=?, progress_text=?,status_change=NOW(),buildend=NOW() WHERE id=?"

#define SQL_GET_EXPIRED_BUILDS "SELECT id,project,revision,agent,attempts FROM build WHERE status='building' AND TIMESTAMPDIFF(MINUTE, status_change, now()) >= ?"

#define SQL_RESTART_BUILD "UPDATE build SET status=?, status_change=NOW(), jobsecret = NULL WHERE id=?"

#define SQL_GET_RELEASES "SELECT id,target,version,revision FROM build INNER JOIN (SELECT max(id) AS id FROM build WHERE status='done' AND project=? GROUP BY target) latest USING (id)"


#define SQL_GET_ARTIFACTS "SELECT id,type,sha1,size,name FROM artifact WHERE build_id = ?"

#define SQL_GET_DELETED_ARTIFACTS "SELECT id,name,storage,payload,project FROM deleted_artifact WHERE error IS NULL LIMIT 1"

#define SQL_DELETE_DELETED_ARTIFACT "DELETE FROM deleted_artifact WHERE id=?"

#define SQL_FAIL_DELETED_ARTIFACT "UPDATE deleted_artifact SET error=? WHERE id=?"

