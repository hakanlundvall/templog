const http = require('@actions/http-client');
let httpc = new http.HttpClient('github-actions');
(async () => {
    let headers = { 'Content-Type': 'application/json' };
    let payload = JSON.stringify({
        run_id: process.env.GITHUB_RUN_ID,
        commit_hash: process.env.COMMIT_HASH
    });
    console.log(`Pushing ${payload}`);
    let res = await httpc.post('https://www.lundvall.info/templog/firmware', payload, headers);
    if (res.message.statusCode != 200) {
        throw new Error(`Failed to push: ${res.message.statusMessage}`);
    }
})();