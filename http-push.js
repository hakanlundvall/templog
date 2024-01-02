const http = require('@actions/http-client');
let httpc = new http.HttpClient('github-actions');
(async () => {
  let res = await httpc.post('http://www.lundvall.info/templog/firmware', JSON.stringify({run_id: process.env.GITHUB_RUN_ID}));
  if (res.message.statusCode != 200) {
    throw new Error(`Failed to push: ${res.message.statusMessage}`);
  }
})();