wget -O - \
    --method=POST http://localhost:8080/measurement \
    --header="Content-Type: application/json" \
    --body-data="{\"id\":\"$1\",\"t\":$2,\"h\":$3,\"p\":$4}"