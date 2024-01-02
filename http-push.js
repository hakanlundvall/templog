const http = require('@actions/http-client');
let httpc = new http.HttpClient('github-actions');
(async () => {
    let headers = { 'Content-Type': 'application/json',
                    'authorization': 'Bearer ' + process.env.TEMPLOG_TOKEN };
    let run_id = process.env.GITHUB_RUN_ID;
    let commit_hash = process.env.COMMIT_HASH;

    
    let res = await httpc.get(`https://api.github.com/repos/hakanlundvall/templog/actions/runs/${run_id}/artifacts`, headers);
    if (res.message.statusCode != 200) {
        throw new Error(`Failed to get artifact: ${res.message.statusMessage}`);
    }
    let artifacts = JSON.parse(await res.readBody()).artifacts;
    console.log(`Found ${artifacts.length} artifacts`);
    console.log(artifacts);
    
    headers = { 'Content-Type': 'application/json'}
    
    let payload = JSON.stringify({
        run_id,
        commit_hash
    });
    console.log(`Pushing ${payload}`);
    
    res = await httpc.post('https://www.lundvall.info/templog/firmware', payload, headers);
    if (res.message.statusCode != 200) {
        throw new Error(`Failed to push: ${res.message.statusMessage}`);
    }
})();