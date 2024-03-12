Nume: Stefan Darius
Grupă: 324CD

Tema 3: Server web asincron

Implementare:

Am pornit de la sample-ul primit, epoll_echo_server.c. Pentru a parsa cererile HTTP (pentru a obține calea unde se
află fișierul cerut) am folosit cod din sample-ul primit, test_get_request_path.c.

Server-ul meu rulează ca o mașină pe stări. Într-o buclă infinită primește notificări de la un epoll, iar pe baza
acestor notificări face acțiuni care îi schimbă starea. El tratează cererile HTTP primite, extrage calea fișierului
cerut și trimite ca răspuns un header HTTP, iar în caz că fișierul chiar există, îl trimite și pe acesta. Când
răspunsul este trimis în totalitate, conexiunea se termină.

Parcursul unei conexiuni server-client:

În momentul în care se trimite o cerere către server pe portul 8888, pe care acesta și-a deschis un socket pe care
ascultă, și pe care l-a adăugat într-o structură de epoll, server-ul este notificat că poate citi de pe socket și
acceptă conexiunea creând un nou socket non-blocant pe care care să primească mesajul. Este creată și o structură 
de conexiune care conține mai multe câmpuri în care se țin stocate informații despre conexiunea respectivă de-a
lungul existenței ei. Socket-ul nou creat este adăugat apoi în epoll pentru a se citi de pe acesta.

Când server-ul este notificat că primește ceva pe un socket ascoiat unei conexiuni, el începe citirea de pe socket.
Pentru că socketul este non-blocant citirea de pe acesta posibil să nu se facă complet (să mai rămână bytes de citit).
Astfel se verifică dacă ce s-a citit până atunci se termină așa cum se termină o cerere HTTP: cu "\r\n\r\n". Dacă nu,
se rămâne în starea STATE_REQUEST_RECEIVED_PARTIALLY. În acest caz nu se modifică nimic legat de socket, deci la 
următoarea notificare dată de epoll pe acest socket se continuă citirea. În cazul în care se ajunge la finalul unei
cereri HTTP, se extrage calea către fișierul cerut și se verifică existența acestuia și categoria din care face parte:
static sau dinamic (separat de stările legate de tratarea unei cereri, există și stările legate de tipul fișierului
cerut). Dacă fișierul nu se poate deschide conexiunea trece în categoria STATE_FILE_ERROR, dacă este un fișier din
folderul static, trece în STATE_FILE_STATIC, iar dacă este din folderul dynamic, trece în STATE_FILE_DYNAMIC. Se 
schimbă socketul în epoll pentru a fi notificat pentru scriere. Se schimbă starea în STATE_REQUEST_RECEIVED.

Când serverul primește notificare de la epoll pentru scrierea pe un socket se verifică prima dată starea fișierului
cerut:
1. STATE_FILE_ERROR: se trimite un header cu mesajul "HTTP/1.0 404 Error open fd!\r\n\r\n" folosind funcția 
	send_header, care trimite acest header pe socket; fiind un socket non-blocant se repetă povestea de la primirea
	cererii, se verifică câți bytes au fost trimiși până atunci cu dimensiunea headerului; dacă s-au trimis mai
	puțini se trece în starea STATE_HEADER_SENT_PARTIALLY, iar socketul rămâne deschis pentru scriere; dacă s-a
	trimis tot headerul, se trece în starea STATE_HEADER_SENT; se închide apoi conexiunea cu acest client.

2. STATE_FILE_STATIC: se trimite un header cu mesajul "HTTP/1.0 200 Static!\r\n\r\n" folosind același mecanism
	ca în cazul unei erori; diferența constă în faptul că după ce se trece în starea STATE_HEADER_SENT, nu se
	închide socketul; la următoarea notificare primită pentru scriere pe acest socket, se începe trimiterea către
	client a fișierului cerut din folderul static; se folosește funcția sendfile care implementează mecanismul
	de zero copying; deoarece această funcție nu garantează trimiterea completă, se procedează ca la celelalte
	transferuri de date implementate de server; când se trimite tot fișierul se închide conexiunea.

3. STATE_FILE_DYNAMIC: se trimite un header cu mesajul "HTTP/1.0 200 Dynamic!\r\n\r\n" folosind același mecanism ca
	în primele 2 cazuri; în starea STATE_HEADER_SENT se începe citirea asincronă de pe disc a fișierului cerut
	în mai multe buffere cu dimensiunea BUFSIZ (se împarte fișierul în mai multe părți și se trimite câte o cerere
	de citire asincronă pentru fiecare parte); se șterge socketul din epoll, și se adaugă în loc un eventfd care
	să fie notificat pe măsură ce aceste citiri inițializate se termină; trece în starea STATE_ASYNC_READ_STARTED;
	pe măsură ce se primesc notificări pe acest eventfd, se verifică dacă s-au încheiat toate citirile care trebuie
	trimise; când se finalizează toate, este adăugat socketul pentru scriere și este șters din epoll eventfd-ul și
	se trece în starea STATE_ASYNC_READ_FINISHED; mai departe se trimit bufferele pe socket, ținând cont de faptul
	că socketii sunt non-blocanti; la final, se șterge conexiunea.

Am creat pentru fiecare conexiune care primește o cerere de fișier din folderul dynamic un io_context potrivit pentru
a susține procesarea concomitentă a cel puțin 1 operație I/O asincronă (în manualul de kernel(1) de Linux spune ca 
io_setup creează un context "POTRIVIT" pentru numărul de evnimente primit ca parametru, dar și o sursă (2) unde spune 
că apelul de sistem creează un context capabil să susțina "CEL PUȚIN" numărul de evenimente primit ca parametru, deci 
am interpretat explicația în sensul că poate procesa concomitent și mai mult de 1 eveniment, iar cu alt parametru nu 
am obținut performanțe mai bune).

Starea STATE_HEADER_SENT_PARTIALLY este setată la 1 pentru a fi egală cu STATE_REQUEST_RECEIVED deoarece funcția 
send_header setează starea conexiunii către aceasta în cazul în care nu se trimite headerul complet și trebuia altfel 
tratată separat în switch-uri și această stare, dar care să facă același lucru.

Bibliografie:
1) https://man7.org/linux/man-pages/man2/io_setup.2.html
2) https://linux.die.net/man/2/io_setup
3) https://ocw.cs.pub.ro/courses/so/laboratoare/laborator-11
