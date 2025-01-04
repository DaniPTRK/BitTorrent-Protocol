# Tema 2 APD 2025 - Ion Daniel 335CC
## BitTorrent Protocol

### Implementare
Pentru a implementa protocolul BitTorrent, am folosit mai multe task-uri MPI,
fiecare task avand asociat 2 thread-uri, unul de download si unul de upload.

Inainte de a crea thread-urile, am extras informatiile din fiserele input asociate
fiecarui task si le-am salvat intr-o structura de date specifica peer-urilor, anume
peer_info. Aceasta structura are urmatorul continut:

```C++
// Info held by a peer.
struct peer_info {
    // Storage for the name of the file and the hashes associated to the file.
    unordered_map<string, vector<string>> *seed_files, *needed_files;
    int rank, numtasks;
    bool active;
};
```

Astfel, avem:
* seed_files, care reprezintă toate fișierele pe care peer-ul le deține complet și
cărora le poate da seed. Cheia map-ului este numele fișierului, iar valorea este
un vector de string-uri, de hash-uri ce conțin informația fișierului;
* needed_files, un map ce conține inițial doar numele fișierelor pe care peer-ul
dorește să le descarce. Pe parcursul descărcărilor, această mapă va primi în
interior hash-urile fișierului asociat;
* rank reprezintă rank-ul task-ului ce reprezintă peer-ul;
* numtasks indică numărul de task-uri/peers ce utilizează tracker-ul;
* active indică dacă peer-ul este sau nu conectat la tracker;
* seed_files și needed_files sunt pointeri întrucât am dorit ca informațiile ce se află
în interiorul celor 2 variabile să împărțite între thread-ul de upload și cel de download.

După ce toate informațiile sunt extrase, în communicate_tracker() are loc comunicarea dintre
peer și tracker, în care peer-ul transmite informațiile sale către tracker. Tracker-ul,
concomitent, în populate_swarm(), extrage informații de la toate celelalte task-uri/peers.
Toate aceste informații sunt salvate ulterior într-o structură tracker_info ce are
următoarea formă:

```C++
// Info held by the tracker.
struct tracker_info{
    // Map which contains the file and the peers who seed it.
    unordered_map<string, unordered_set<int>> swarms;

    // Map which contains the file and the hashes associated.
    unordered_map<string, string> files_info;
};
```

Astfel, avem:
* swarms, o mapă ce conține ca și cheie numele unui fișier și toți clienții care sunt
seeds pentru acest fișier sau cei care descarcă fișierul și au câteva segmente din fișier;
* files_info, o mapă ce conține un fișier, cât și conținutul acestuia. Am decis ca
informația să fie stocată, de această dată, sub forma unui string ce conține hash-uri
alipite.

În populate_swarm(), tracker-ul intră într-un loop în care așteaptă de la fiecare client
informații despre fișierele pe care le au. În cazul în care tracker-ul nu recunoaște
numele unui fișier, adică fișierul nu este în files_info, tracker-ul trimite un NACK către
client și îi cere hash-urile acestui fișier (motivul pentru care am decis să alipesc
hash-urile este acela că mi-a fost mult mai ușor să trimit prin MPI_Send un string lung
decât să trimit fragmentat string-urile + am avut garanția că se trimite totul safe). Dacă
tracker-ul are fișierul, trimite un ACK și continuă popularea swarm-urilor. Când un client
a trimis tot ce știe, trimite un DONE, iar tracker-ul ține cont de numărul de mesaje de
acest tip, astfel că după ce se trimit destule DONE-uri, iese din loop și trimite tuturor
clienților un ACK pentru a putea începe download-ul / upload-ul.

În tracker, se așteaptă constant un mesaj de la orice sursă, de pe orice tag. Dacă mesajul
este "FIN", înseamnă că un client a terminat descărcarea de fișiere și doar va da seed la
ce fișiere deține. Dacă mesajul nu este de acest fel, clientul este trecut în swarm-ul
fișierului din mesaj și îi sunt trimise hash-ul și swarm-urile. Când toți clienții au
terminat de descărcat, tracker-ul iese din loop și trimite un "FIN" la toate thread-urile
de upload pentru ca acestea să își încheie activitatea.

În clienți, thread-urile de download și upload comunică constant între ele prin Send și
Recv. Thread-urile primesc ca argument structura peer_info asociată task-ului părinte.

În download, se verifică după fiecare descărcarea dacă numărul de segmente este mai mare cu
10, dacă da, are loc o actualizare a swarm-urilor în clienți. De asemenea, după fiecare
descărcare, download-ul trimite către uploaderi o cerere pentru a obține numărul de
uploads pe care îl are fiecare thread. După ce primește numărul de uploads, sortează valorile
primite și va alege, pentru fișierele pe care vrea să le descarce, pe acei peers care au numărul
de descărcări minim, pentru a putea distribui cât mai eficient și mai balansat munca. După
ce se obține numărul de uploads, download-ul trece prin toate fișierele pe care task-ul
le dorește și caută în swarm peers/seeds ce pot oferi segmente din fișier. Astfel,
download trimite o instrucțiune "EXTRACT" către upload, după care trimite numele fișierului
și hash-ul pe care-l caută. Dacă seed/peer deține acest hash, trimite un ACK, în caz contrar
NACK. Dacă avem ACK, creștem nr de segmente descărcate și verificăm dacă fișierul a fost
descărcat complet. Dacă este descărcat complet, în funcția write_logs() se creează un fișier
ce scrie informațiile salvate în needed_files, informații ce vor fi trecute după aceea în
seed_files. Dacă avem NACK, se continuă căutarea. Download-ul își dă seama că munca este
terminată când size-ul lui needed_files este 0, adică atunci când peer-ul nu mai are ce
descărca.

În upload, se așteaptă un mesaj de la orice download/tracker ce are tag-ul TO_UPLOAD, 2.
Există 3 tipuri de mesaje pe care upload-ul le poate primi:
* FIN - primit de la tracker, înseamnă că uploader-ul își poate încheia activitatea întrucât
toți clienții au terminat de descărcat;
* UPLOADS - primit de la un download prin TO_UPLOAD, trimite cu tag-ul TO_DOWNLOAD nr
de upload-uri pe care îl are peer-ul;
* EXTRACT - primit de la un download, după acest mesaj urmează să fie primite încă 2 mesaje,
un nume de fișier și un hash.

În cazul EXTRACT, upload-ul verifică dacă fișierul este în seed_files sau în needed_files.
Dacă este seeded, se trimite ACK direct, având tot fișierul, dacă se află în needed,
upload-ul va căuta prin toate hash-urile descărcate momentan și va trimite un ACK dacă 
peer-ul are hash-ul descărcat. În caz contrar, se trimite un NACK. La fiecare ACK, nr
de upload-uri crește.

După închiderea thread-urilor, tracker-ul și clienții încheie activitatea.