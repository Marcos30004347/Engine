/probe stats/{for(i=1;i<=NF;i++){if($i~/^frame=/){split($i,b,"=");f=b[2]+0}} if(f<55)next;
for(i=1;i<=NF;i++){if($i~/=/&&$i!~/^frame=/){split($i,c,"=");v[c[1]]=c[2]; if(!(c[1] in seen)){seen[c[1]]=1;order[++k]=c[1]}}}
n++; for(j=1;j<=k;j++){key=order[j]; s[key]+=v[key]; if(n==1||v[key]<mn[key])mn[key]=v[key]; if(n==1||v[key]>mx[key])mx[key]=v[key]}}
END{printf "%s frames=%d\n",FILENAME,n; for(j=1;j<=k;j++){key=order[j]; printf "  %-20s mean=%-10.4f min=%-9.4f max=%-9.4f swing=%.1f%%\n", key, s[key]/n, mn[key], mx[key], 100.0*(mx[key]-mn[key])/(s[key]/n)}}
